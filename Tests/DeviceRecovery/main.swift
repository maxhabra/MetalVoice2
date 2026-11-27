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

func mic(_ uid: String, defaultInput: Bool = false, virtual: Bool = false) -> AudioDevice {
    AudioDevice(id: 0, uid: uid, name: uid, inputs: 1, outputs: 0, sampleRate: 48000,
                bufferFrames: 512, isVirtual: virtual, isDefaultInput: defaultInput)
}
let usb = mic("usb"), builtIn = mic("built-in", defaultInput: true)
let virtual = mic("virtual", defaultInput: true, virtual: true)
func pick(_ devices: [AudioDevice], selected: String, lastDefault: String?) -> String? {
    AudioDevice.microphone(in: devices, selectedUID: selected, previousDefaultUID: lastDefault)?.uid
}
precondition(pick([usb, builtIn], selected: "usb", lastDefault: "usb") == "built-in")
precondition(pick([usb, builtIn], selected: "usb", lastDefault: "built-in") == "usb")
precondition(pick([usb, builtIn], selected: "usb", lastDefault: nil) == "usb")
precondition(pick([builtIn], selected: "usb", lastDefault: "built-in") == "built-in")
precondition(pick([], selected: "usb", lastDefault: "usb") == nil)
precondition(pick([builtIn], selected: "usb", lastDefault: "") == "built-in")
precondition(pick([virtual, usb], selected: "usb", lastDefault: "usb") == "usb")
precondition(pick([virtual], selected: "usb", lastDefault: "usb") == nil)
print("PASS default handoff, manual selection, unplug, delayed replacement and virtual input exclusion")
