import AudioCore
import Foundation

let now = Date(timeIntervalSince1970: 1000)
var state = DeviceRecovery()
precondition(!state.shouldRetry(at: now))
state.request()
state.waitForDevice(now: now)
precondition(!state.shouldRetry(at: now) && state.shouldRetry(at: now.addingTimeInterval(1)))
state.completedStart(Int32(MVStartDeviceNotReady), now: now)
precondition(state.requested && state.shouldRetry(at: now.addingTimeInterval(2)))
for _ in 0..<5 { state.completedStart(Int32(MVStartDeviceNotReady), now: now) }
precondition(state.retryAt == now.addingTimeInterval(8))
state.completedStart(Int32(MVStartOK), now: now)
precondition(state.requested)
state.waitForDevice(now: now)
precondition(state.retryAt == now.addingTimeInterval(1))
state.cancel()
precondition(!state.shouldRetry(at: .distantFuture))
state.request()
state.completedStart(Int32(MVStartFailed), now: now)
precondition(!state.shouldRetry(at: .distantFuture))
print("PASS reconnect after transient open, bounded retries, explicit Stop and permanent failure")
