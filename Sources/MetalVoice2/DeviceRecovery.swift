import AudioCore
import Foundation

// Controller-owned intent survives a temporary HAL failure, device loss and sleep.
// All state transitions run on the main thread.
struct DeviceRecovery {
    private(set) var requested = false
    var retryAt = Date.distantPast
    private var delay: TimeInterval = 1

    mutating func request() {
        if !requested { delay = 1 }
        requested = true
    }

    mutating func cancel() { requested = false }

    mutating func waitForDevice(now: Date = Date()) {
        retryAt = now.addingTimeInterval(delay)
        delay = min(8, delay * 2)
    }

    mutating func completedStart(_ result: Int32, now: Date = Date()) {
        switch result {
        case Int32(MVStartOK): delay = 1
        case Int32(MVStartDeviceNotReady): waitForDevice(now: now)
        default: cancel()
        }
    }

    func shouldRetry(at now: Date = Date()) -> Bool {
        requested && now >= retryAt
    }
}
