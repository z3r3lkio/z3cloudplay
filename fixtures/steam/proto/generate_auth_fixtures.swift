import Foundation
import SteamProto
import SwiftProtobuf

let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
func write<M: SwiftProtobuf.Message>(_ name: String, _ message: M) throws {
    try message.serializedData().write(to: output.appendingPathComponent(name))
}

var device = CAuthentication_DeviceDetails()
device.deviceFriendlyName = "Playden Synthetic Device"
device.platformType = .kEauthTokenPlatformTypeSteamClient
device.osType = 0
var beginRequest = CAuthentication_BeginAuthSessionViaQR_Request()
beginRequest.deviceDetails = device
beginRequest.websiteID = "Client"
try write("begin_request.pb", beginRequest)

var deviceConfirmation = CAuthentication_AllowedConfirmation()
deviceConfirmation.confirmationType = .kEauthSessionGuardTypeDeviceConfirmation
var emailConfirmation = CAuthentication_AllowedConfirmation()
emailConfirmation.confirmationType = .kEauthSessionGuardTypeEmailConfirmation
emailConfirmation.associatedMessage = "example.invalid"
var beginResponse = CAuthentication_BeginAuthSessionViaQR_Response()
beginResponse.clientID = 0x0102030405060708
beginResponse.challengeURL = "https://synthetic.invalid/qr/challenge-a"
beginResponse.requestID = Data([0x10, 0x20, 0x30, 0x40, 0x50])
beginResponse.interval = 2.5
beginResponse.allowedConfirmations = [deviceConfirmation, emailConfirmation]
beginResponse.version = 3
try write("begin_response.pb", beginResponse)

var pollRequest = CAuthentication_PollAuthSessionStatus_Request()
pollRequest.clientID = 0x0102030405060708
pollRequest.requestID = Data([0x10, 0x20, 0x30, 0x40, 0x50])
try write("poll_request.pb", pollRequest)

var pending = CAuthentication_PollAuthSessionStatus_Response()
pending.newClientID = 0x1112131415161718
pending.newChallengeURL = "https://synthetic.invalid/qr/challenge-b"
pending.hadRemoteInteraction = true
try write("poll_pending_response.pb", pending)

var success = CAuthentication_PollAuthSessionStatus_Response()
success.refreshToken = "synthetic.refresh.token"
success.accessToken = "synthetic.access.token"
success.hadRemoteInteraction = true
success.accountName = "synthetic_account"
success.newGuardData = "synthetic-guard-data"
success.agreementSessionURL = "https://synthetic.invalid/agreement"
try write("poll_success_response.pb", success)

var renewRequest = CAuthentication_AccessToken_GenerateForApp_Request()
renewRequest.refreshToken = "synthetic.refresh.token"
renewRequest.steamid = 0x0110000100001234
renewRequest.renewalType = .kEtokenRenewalTypeNone
try write("renew_request.pb", renewRequest)

var renewResponse = CAuthentication_AccessToken_GenerateForApp_Response()
renewResponse.accessToken = "synthetic.renewed.access.token"
renewResponse.refreshToken = "synthetic.rotated.refresh.token"
try write("renew_response.pb", renewResponse)

func load<M: SwiftProtobuf.Message>(_ name: String, as type: M.Type) throws -> M {
    let data = try Data(contentsOf: output.appendingPathComponent(name))
    return try M(serializedBytes: data)
}
let decodedBegin = try load("begin_response.pb", as: CAuthentication_BeginAuthSessionViaQR_Response.self)
precondition(decodedBegin.clientID == 0x0102030405060708 && decodedBegin.allowedConfirmations.count == 2)
let decodedPending = try load("poll_pending_response.pb", as: CAuthentication_PollAuthSessionStatus_Response.self)
precondition(decodedPending.hasNewClientID && decodedPending.hasNewChallengeURL)
let decodedSuccess = try load("poll_success_response.pb", as: CAuthentication_PollAuthSessionStatus_Response.self)
precondition(decodedSuccess.refreshToken == "synthetic.refresh.token" && decodedSuccess.accountName == "synthetic_account")
let decodedRenew = try load("renew_response.pb", as: CAuthentication_AccessToken_GenerateForApp_Response.self)
precondition(decodedRenew.accessToken == "synthetic.renewed.access.token")
