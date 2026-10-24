// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import Foundation
import SwiftUI
import UIKit

/// The Swift face of EdenCoreBridge. Owns the security-scoped resource for the session
/// and the JIT gate; owns no libretro state of its own.
@MainActor
final class EmulatorSession: ObservableObject {

    enum Phase: Equatable {
        case idle
        case presenting
        case starting
        case running
        case stopping
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var status: String = "idle"
    @Published private(set) var framesPerSecond: Double?
    @Published private(set) var noNewFrames = false
    @Published private(set) var isPaused = false
    @Published private(set) var currentGame: GameEntry?

    /// Set when a launch is refused because JIT is not available, so the UI can present
    /// JITRequiredView instead of a generic error.
    @Published var jitRefusal: JITStatus?

    var isRunning: Bool {
        if case .running = phase { return true }
        return false
    }

    /// The security-scoped URL held open for the whole session.
    ///
    /// docs/IOS_PORT_NOTES.md #4: "A ROM outside the container needs its
    /// security-scoped resource held for the whole session, not just at open time."
    /// The core opens the file lazily and repeatedly - retro_get_system_info sets
    /// need_fullpath = true (retro_core.cpp:349) and Core::System::Load walks split
    /// dumps and NCA containers long after retro_load_game returned - so releasing the
    /// scope at the end of the launch call would revoke access midway through play.
    private var scopedURL: URL?
    private var pendingGame: GameEntry?
    private var lastPresentedFrameCount: UInt64 = 0
    private var lastFrameSampleDate: Date?
    private var lastNewFrameDate: Date?

    private var pollTimer: AnyCancellable?

    // MARK: - launching

    func start(_ game: GameEntry) {
        guard case .idle = phase else { return }

        // THE JIT GATE, and it goes here - before anything touches the core.
        //
        // docs/IOS_PORT_NOTES.md: "There is no interpreter. ... No JIT does not mean
        // slow emulation; it means no emulation." Without this check the first thing
        // the user sees is the app disappearing partway into a load, because dynarmic
        // dies at its first code emission and a code-signing violation on iOS is an
        // uncatchable signal.
        //
        // Checked on the main thread before the emulation thread is created, rather
        // than inside the bridge: by the time the bridge is running, presenting a
        // SwiftUI explanation means hopping back here anyway.
        let jitStatus = JITStatus.current()
        guard jitStatus.permitsEmulation else {
            jitRefusal = jitStatus
            return
        }
        // Hold the scope BEFORE the bridge is told the path.
        if game.needsSecurityScope {
            guard game.url.startAccessingSecurityScopedResource() else {
                phase = .failed("iOS refused access to that file. Re-import it from Files.")
                return
            }
            scopedURL = game.url
        }

        currentGame = game
        pendingGame = game
        phase = .presenting
        status = "Preparing the Metal surface..."
        jitRefusal = nil
    }

    /// Called after EmulationView appears. SwiftUI can invoke onAppear before UIKit's
    /// first non-zero layout, so wait for the bridge's actual layer attachment.
    func emulationSurfaceDidAppear() {
        guard case .presenting = phase else { return }
        Task { @MainActor in
            for _ in 0..<50 {
                guard case .presenting = self.phase else { return }
                let surface = EdenMetalLayerView.shared
                let size = surface.physicalPixelSize
                if surface.window != nil && size.width > 0 && size.height > 0 && eden_bridge_has_layer() {
                    self.startPendingGame()
                    return
                }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            guard case .presenting = phase else { return }
            releaseScope()
            pendingGame = nil
            let message = "The emulation view appeared, but its CAMetalLayer was not attached. Close and try again."
            phase = .failed(message)
            status = message
        }
    }

    private func startPendingGame() {
        guard case .presenting = phase, let game = pendingGame else { return }

        // The arena must pass an actual RX execution test, not only the debugger
        // mapping handshake. Recheck at the last boundary before touching the core.
        let jitStatus = JITStatus.current()
        guard jitStatus.permitsEmulation else {
            releaseScope()
            pendingGame = nil
            currentGame = nil
            phase = .idle
            jitRefusal = jitStatus
            return
        }
        let surface = EdenMetalLayerView.shared
        guard surface.window != nil, eden_bridge_has_layer() else { return }

        // No more diagnostic mappings after Dynarmic starts allocating code.
        eden_jit_lock_verdict()
        phase = .starting
        status = "starting"
        framesPerSecond = nil
        noNewFrames = false
        lastPresentedFrameCount = 0
        lastFrameSampleDate = nil
        lastNewFrameDate = nil

        // Take one immutable snapshot of the persisted frontend choices before the
        // bridge creates its emulation thread. The bridge validates and publishes
        // these values when retro_set_environment declares its options, before
        // retro_init applies the latch-once core settings.
        let defaults = UserDefaults.standard
        let options: [(String, String)] = [
            ("eden_resolution", defaults.string(forKey: "eden_resolution") ?? "0.5x"),
            ("eden_use_docked", defaults.string(forKey: "eden_use_docked") ?? "No"),
            ("eden_multi_core", defaults.string(forKey: "eden_multi_core") ?? "Disabled"),
            ("eden_async_gpu", defaults.string(forKey: "eden_async_gpu") ?? "Disabled"),
            ("eden_memory_layout", defaults.string(forKey: "eden_memory_layout") ?? "4GB"),
            ("eden_speed_limit", defaults.string(forKey: "eden_speed_limit") ?? "Enabled"),
        ]
        let optionsAccepted = options.allSatisfy { option in
            option.0.withCString { keyPointer in
                option.1.withCString { valuePointer in
                    eden_bridge_set_option(keyPointer, valuePointer)
                }
            }
        }
        guard optionsAccepted else {
            let message = "Saved core settings were invalid; review Settings and try again."
            phase = .failed(message)
            status = message
            releaseScope()
            pendingGame = nil
            return
        }

        let started = game.url.path.withCString { rom in
            EdenPaths.dataRootPath.withCString { root in
                eden_bridge_start(rom, root)
            }
        }

        guard started else {
            releaseScope()
            pendingGame = nil
            phase = .failed(readStatus())
            status = readStatus()
            return
        }

        pendingGame = nil
        UIApplication.shared.isIdleTimerDisabled = true
        beginPolling()
    }

    func stop() {
        guard phase != .idle else { return }
        phase = .stopping
        status = "shutting down"

        // eden_bridge_stop joins the emulation thread, and that join can be long: a
        // stop requested while retro_load_game is still inside Core::System::Load
        // cannot take effect until the load returns, because there is no cancellation
        // path through Eden's loader. Doing it on a background queue keeps the main
        // thread responsive - which is the same rule as #3 in IOS_PORT_NOTES.md, for
        // the same reason.
        Task.detached(priority: .userInitiated) {
            eden_bridge_stop()
            await MainActor.run {
                self.releaseScope()
                self.endPolling()
                self.phase = .idle
                self.status = self.readStatus()
                self.currentGame = nil
                self.pendingGame = nil
                self.framesPerSecond = nil
                self.noNewFrames = false
                self.isPaused = false
                UIApplication.shared.isIdleTimerDisabled = false
            }
        }
    }

    func togglePause() {
        eden_bridge_set_paused(!isPaused)
    }

    private func releaseScope() {
        if let url = scopedURL {
            url.stopAccessingSecurityScopedResource()
            scopedURL = nil
        }
    }

    // MARK: - polling
    //
    // Pull, not push. The bridge could take a callback, but every one of these values
    // is produced on the emulation thread and consumed by SwiftUI on the main thread,
    // and a 4 Hz poll of three scalars is cheaper and far easier to reason about than
    // marshalling callbacks across that boundary sixty times a second.

    private func beginPolling() {
        pollTimer = Timer.publish(every: 0.25, on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in
                self?.refresh()
            }
    }

    private func endPolling() {
        pollTimer?.cancel()
        pollTimer = nil
    }

    private func refresh() {
        status = readStatus()
        let pausedNow = eden_bridge_is_paused()
        if pausedNow != isPaused {
            lastFrameSampleDate = nil
            lastNewFrameDate = nil
            framesPerSecond = nil
            noNewFrames = false
        }
        isPaused = pausedNow

        switch eden_bridge_state() {
        case EdenBridgeStateRunning:
            phase = .running
            if isPaused {
                framesPerSecond = nil
                noNewFrames = false
            } else {
                refreshPresentedFrameRate()
            }
        case EdenBridgeStateStarting:
            phase = .starting
        case EdenBridgeStateStopping:
            phase = .stopping
        case EdenBridgeStateFailed:
            phase = .failed(status)
            endPolling()
            releaseScope()
            UIApplication.shared.isIdleTimerDisabled = false
        case EdenBridgeStateIdle:
            // The emulation thread finished cleanup after a core shutdown callback
            // or an explicit frontend stop. The bridge status retains the classified
            // reason for the termination.
            if phase != .idle {
                phase = .idle
                releaseScope()
                endPolling()
                currentGame = nil
                UIApplication.shared.isIdleTimerDisabled = false
            }
        default:
            break
        }
    }

    private func refreshPresentedFrameRate() {
        let now = Date()
        let count = UInt64(eden_bridge_presented_frame_count())
        if lastFrameSampleDate == nil {
            lastPresentedFrameCount = count
            lastFrameSampleDate = now
            lastNewFrameDate = now
            return
        }
        guard count >= lastPresentedFrameCount else {
            // retro_init resets the core's per-session counter.
            lastPresentedFrameCount = count
            lastFrameSampleDate = now
            lastNewFrameDate = nil
            framesPerSecond = nil
            noNewFrames = false
            return
        }

        let delta = count - lastPresentedFrameCount
        if let sampleDate = lastFrameSampleDate, delta > 0 {
            let elapsed = now.timeIntervalSince(sampleDate)
            if elapsed > 0 { framesPerSecond = Double(delta) / elapsed }
        }
        if delta > 0 {
            lastNewFrameDate = now
            noNewFrames = false
        } else if let lastNewFrameDate {
            noNewFrames = now.timeIntervalSince(lastNewFrameDate) >= 3
            if noNewFrames { framesPerSecond = nil }
        } else if let sampleDate = lastFrameSampleDate {
            noNewFrames = now.timeIntervalSince(sampleDate) >= 3
        }
        lastPresentedFrameCount = count
        lastFrameSampleDate = now
    }

    private func readStatus() -> String {
        var buffer = [CChar](repeating: 0, count: 512)
        eden_bridge_copy_status(&buffer, buffer.count)
        return String(cString: buffer)
    }

}
