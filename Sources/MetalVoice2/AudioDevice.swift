struct AudioDevice: Identifiable, Equatable {
    let id: UInt32
    let uid: String
    let name: String
    let inputs: Int
    let outputs: Int
    let sampleRate: Double
    let bufferFrames: UInt32
    let isVirtual: Bool
    let isDefaultInput: Bool

    var isMicrophone: Bool { inputs > 0 && !isVirtual && !uid.isEmpty }

    static func microphone(in devices: [Self], selectedUID: String, previousDefaultUID: String?) -> Self? {
        let inputs = devices.filter(\.isMicrophone)
        let systemDefault = inputs.first(where: \.isDefaultInput)
        if let systemDefault, let previousDefaultUID, systemDefault.uid != previousDefaultUID {
            return systemDefault
        }
        if let selected = inputs.first(where: { $0.uid == selectedUID }) { return selected }
        return systemDefault ?? (selectedUID.isEmpty ? inputs.first : nil)
    }
}
