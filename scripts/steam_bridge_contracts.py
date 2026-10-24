"""Reviewed direction/extent contracts for cross-process SDK parameters.

Never infer buffer direction merely from a pointer or a parameter name. An API
not present here retains an explicit unsupported path until its ownership and
lifetime are understood. Extents come from the original Valve signatures.
"""
SCALAR_OUTPUTS = {
 "ISteamClient": "CreateLocalUser CreateGlobalUser",
 "ISteamUserStats": "GetStat GetAchievement GetAchievementAndUnlockTime GetGlobalStat GetUserStat GetUserAchievement GetUserAchievementAndUnlockTime GetAchievementAchievedPercent GetAchievementProgressLimits GetGroupAchievement GetDownloadedLeaderboardEntry GetMostAchievedAchievementInfo GetNextMostAchievedAchievementInfo",
 "ISteamUtils": "GetImageSize GetCSERIPPort IsAPICallCompleted",
 "ISteamApps": "BGetDLCDataByIndex GetDlcDownloadProgress BIsTimedTrial GetNumBetas GetBetaInfo",
 "ISteamFriends": "GetFriendGamePlayed GetFriendGamePlayed2 Deprecated_GetFriendGamePlayed GetClanActivityCounts GetFriendMessage GetClanChatMessage GetChatMessage",
 "ISteamRemoteStorage": "GetQuota GetFileNameAndSize GetUGCDownloadProgress GetLocalFileChange",
 "ISteamHTTP": "SendHTTPRequest SendHTTPRequestAndStreamResponse GetHTTPResponseHeaderSize GetHTTPResponseBodySize GetHTTPDownloadProgressPct GetHTTPRequestWasTimedOut",
 "ISteamInput": "GetDeviceBindingRevision",
 "ISteamController": "GetDeviceBindingRevision GetControllerBindingRevision GetControllerState",
 "ISteamControllerION": "GetControllerState",
 "ISteamNetworking": "IsP2PPacketAvailable ReadP2PPacket GetP2PSessionState IsDataAvailableOnSocket RetrieveDataFromSocket IsDataAvailable RetrieveData GetSocketInfo GetListenSocketInfo",
 "ISteamUser": "GetAvailableVoice GetVoice GetCompressedVoice DecompressVoice GetRegistryInt GetGuestPassToGiveInfo GetGuestPassToRedeemInfo",
 "ISteamInventory": "GetAllItems GetItemsByID DeserializeResult GenerateItems GrantPromoItems AddPromoItem AddPromoItems ConsumeItem ExchangeItems TransferItemQuantity TriggerItemDrop TradeItems GetItemPrice SubmitUpdateProperties InspectItem",
 "ISteamUGC": "GetQueryUGCResult GetItemUpdateProgress GetItemInstallInfo GetItemUpdateInfo GetItemDownloadInfo GetQueryUGCStatistic GetQueryUGCAdditionalPreview",
 "ISteamUnifiedMessages": "GetMethodResponseInfo",
 "ISteamVideo": "IsBroadcasting",
 "ISteamRemotePlay": "BGetSessionClientResolution",
 "ISteamGameCoordinator": "IsMessageAvailable RetrieveMessage",
 "ISteamGameServer": "GSGetSteam2GetEncryptionKeyToSendToNewClient GSCreateUnauthenticatedUser SendUserConnectAndAuthenticate SendUserConnectAndAuthenticate_DEPRECATED GetAuthSessionTicket GetNextOutgoingPacket",
 "ISteamGameServerStats": "GetUserStat GetUserAchievement",
 "ISteamGameStats": "AddNewRow",
 "ISteamMasterServerUpdater": "GetNextOutgoingPacket",
 "ISteamMatchmaking": "GetFavoriteGame GetFavoriteGame2 GetLobbyChatEntry GetLobbyGameServer",
 "ISteamNetworkingSockets": "GetConnectionInfo GetQuickConnectionStatus GetListenSocketAddress CreateSocketPair GetIdentity ReceivedRelayAuthTicket FindRelayAuthTicketForServer GetHostedDedicatedServerAddress GetAuthenticationStatus SendMessageToConnection GetConnectionRealTimeStatus GetFakeIP GetRemoteFakeIPForConnection",
 "ISteamNetworkingMessages": "GetSessionConnectionInfo",
 "ISteamNetworkingUtils": "GetLocalPingLocation ParsePingLocationString GetPingToDataCenter SteamNetworkingIPAddr_ParseString SteamNetworkingIdentity_ParseString GetRelayNetworkStatus GetRealIdentityForFakeIP GetConfigValueInfo",
 "ISteamParties": "GetBeaconDetails GetNumAvailableBeaconLocations",
}
SCALAR_OUTPUTS = {(family, method) for family, methods in SCALAR_OUTPUTS.items() for method in methods.split()}

def spans(family, method, types):
    # Index -> (input/output/text, extent expression, return is used count).
    key = family + "." + method
    plans = {
      "ISteamApps.BGetDLCDataByIndex": {3:("text", "arg4", False)},
      "ISteamApps.GetCurrentBetaName": {0:("text", "arg1", False)},
      "ISteamApps.GetLaunchCommandLine": {0:("text", "arg1", False)},
      "ISteamApps.GetAppData": {2:("text", "arg3", False)},
      "ISteamApps.GetBetaInfo": {3:("text", "arg4", False),5:("text", "arg6", False)},
      "ISteamRemoteStorage.FileWrite": {1:("input", "arg2", False)},
      "ISteamRemoteStorage.FileRead": {1:("output", "arg2", True)},
      "ISteamRemoteStorage.FileWriteAsync": {1:("input", "arg2", False)},
      "ISteamRemoteStorage.FileReadAsyncComplete": {1:("output", "arg2", False)},
      "ISteamRemoteStorage.FileWriteStreamWriteChunk": {1:("input", "arg2", False)},
      "ISteamRemoteStorage.UGCRead": {1:("output", "arg2", True)},
      "ISteamUser.BeginAuthSession": {0:("input", "arg1", False)},
      "ISteamUser.RequestEncryptedAppTicket": {0:("input", "arg1", False)},
      "ISteamUser.SetSteam2Ticket": {0:("input", "arg1", False)},
      "ISteamUser.GetSteamGameConnectToken": {0:("output", "arg1", True)},
      "ISteamUser.GetRegistryString": {2:("text", "arg3", False)},
      "ISteamUser.GetGuestPassToRedeemSenderAddress": {1:("text", "arg2", False)},
      "ISteamUser.GetGuestPassToRedeemSenderName": {1:("text", "arg2", False)},
      "ISteamUtils.GetImageRGBA": {1:("output", "arg2", False)},
      "ISteamUtils.GetEnteredGamepadTextInput": {0:("text", "arg1", False)},
      "ISteamFriends.GetFriendsGroupMembersList": {1:("output", "arg2", False)},
      "ISteamFriends.DownloadClanActivityCounts": {0:("input", "arg1", False)},
      "ISteamFriends.GetFriendMessage": {2:("output", "arg3", True)},
      "ISteamFriends.GetClanChatMessage": {2:("output", "arg3", True)},
      "ISteamFriends.GetChatMessage": {2:("output", "arg3", True)},
      "ISteamUserStats.DownloadLeaderboardEntriesForUsers": {1:("input", "arg2", False)},
      "ISteamUserStats.GetDownloadedLeaderboardEntry": {3:("output", "arg4", False)},
      "ISteamUserStats.GetMostAchievedAchievementInfo": {0:("text", "arg1", False)},
      "ISteamUserStats.GetNextMostAchievedAchievementInfo": {1:("text", "arg2", False)},
      "ISteamHTTP.GetHTTPResponseHeaderValue": {2:("output", "arg3", False)},
      "ISteamHTTP.GetHTTPResponseBodyData": {1:("output", "arg2", False)},
      "ISteamHTTP.GetHTTPStreamingResponseBodyData": {2:("output", "arg3", False)},
      "ISteamHTTP.SetHTTPRequestRawPostBody": {2:("input", "arg3", False)},
      "ISteamNetworking.SendP2PPacket": {1:("input", "arg2", False)},
      "ISteamNetworking.ReadP2PPacket": {0:("output", "arg1", False)},
      "ISteamNetworking.SendDataOnSocket": {1:("input", "arg2", False)},
      "ISteamNetworking.RetrieveDataFromSocket": {1:("output", "arg2", False)},
      "ISteamNetworking.RetrieveData": {1:("output", "arg2", False)},
      "ISteamAppList.GetInstalledApps": {0:("output", "arg1", True)},
      "ISteamAppList.GetAppName": {1:("text", "arg2", False)},
      "ISteamInventory.GetItemsByID": {1:("input", "arg2", False)},
      "ISteamInventory.DeserializeResult": {1:("input", "arg2", False)},
      "ISteamInventory.GenerateItems": {1:("input", "arg3", False),2:("input", "arg3", False)},
      "ISteamInventory.AddPromoItems": {1:("input", "arg2", False)},
      "ISteamInventory.ExchangeItems": {1:("input", "arg3", False),2:("input", "arg3", False),4:("input", "arg6", False),5:("input", "arg6", False)},
      "ISteamInventory.TradeItems": {2:("input", "arg4", False),3:("input", "arg4", False),5:("input", "arg7", False),6:("input", "arg7", False)},
      "ISteamInventory.StartPurchase": {0:("input", "arg2", False),1:("input", "arg2", False)},
      "ISteamMusicRemote.SetPNGIcon_64x64": {0:("input", "arg1", False)},
      "ISteamMusicRemote.UpdateCurrentEntryCoverArt": {0:("input", "arg1", False)},
      "ISteamRemotePlay.CreateMouseCursor": {4:("input", "arg5", False)},
      "ISteamScreenshots.WriteScreenshot": {0:("input", "arg1", False)},
      "ISteamUGC.CreateQueryUGCDetailsRequest": {0:("input", "arg1", False)},
      "ISteamUGC.GetQueryUGCPreviewURL": {2:("text", "arg3", False)},
      "ISteamUGC.GetQueryUGCMetadata": {2:("text", "arg3", False)},
      "ISteamUGC.GetQueryUGCChildren": {2:("output", "arg3", False)},
      "ISteamUGC.StartPlaytimeTracking": {0:("input", "arg1", False)},
      "ISteamUGC.StopPlaytimeTracking": {0:("input", "arg1", False)},
      "ISteamUGC.GetQueryUGCTag": {3:("text", "arg4", False)},
      "ISteamUGC.GetQueryUGCTagDisplayName": {3:("text", "arg4", False)},
      "ISteamUGC.GetQueryUGCContentDescriptors": {2:("output", "arg3", True)},
      "ISteamUGC.GetUserContentDescriptorPreferences": {0:("output", "arg1", True)},
      "ISteamUGC.GetSupportedGameVersionData": {3:("text", "arg5", False),4:("text", "arg5", False)},
      "ISteamUGC.SetItemsDisabledLocally": {0:("input", "arg1", False)},
      "ISteamUGC.SetSubscriptionsLoadOrder": {0:("input", "arg1", False)},
      "ISteamUnifiedMessages.SendMethod": {1:("input", "arg2", False)},
      "ISteamUnifiedMessages.GetMethodResponseData": {1:("output", "arg2", False)},
      "ISteamUnifiedMessages.SendNotification": {1:("input", "arg2", False)},
      "ISteamGameCoordinator.SendMessage": {1:("input", "arg2", False)},
      "ISteamGameCoordinator.RetrieveMessage": {1:("output", "arg2", False)},
      "ISteamMasterServerUpdater.GetMasterServerAddress": {1:("text", "arg2", False)},
      "ISteamGameSearch.RetrieveConnectionDetails": {1:("text", "arg2", False)},
      "ISteamMatchmaking.SendLobbyChatMsg": {1:("input", "arg2", False)},
      "ISteamMatchmaking.GetLobbyChatEntry": {3:("output", "arg4", True)},
      "ISteamMatchmaking.GetLobbyDataByIndex": {2:("text", "arg3", False),4:("text", "arg5", False)},
      "ISteamNetworkingMessages.SendMessageToUser": {1:("input", "arg2", False)},
      "ISteamNetworkingSockets.GetConnectionName": {1:("text", "arg2", False)},
      "ISteamNetworkingSockets.GetDetailedConnectionStatus": {1:("text", "arg2", False)},
      "ISteamNetworkingSockets.SendMessageToConnection": {1:("input", "arg2", False)},
      "ISteamNetworkingSockets.ConfigureConnectionLanes": {2:("input", "arg1", False),3:("input", "arg1", False)},
      "ISteamNetworkingSockets.GetConnectionRealTimeStatus": {3:("output", "arg2", False)},
      "ISteamNetworkingSockets.ReceivedRelayAuthTicket": {0:("input", "arg1", False)},
      "ISteamNetworkingSocketsSerialized.SendP2PRendezvous": {2:("input", "arg3", False)},
      "ISteamNetworkingSocketsSerialized.GetNetworkConfigJSON": {0:("output", "arg1", True)},
      "ISteamNetworkingSocketsSerialized.CacheRelayTicket": {0:("input", "arg1", False)},
      "ISteamNetworkingSocketsSerialized.GetCachedRelayTicket": {1:("output", "arg2", True)},
      "ISteamNetworkingSocketsSerialized.PostConnectionStateMsg": {0:("input", "arg1", False)},
      "ISteamNetworkingUtils.ConvertPingLocationToString": {1:("text", "arg2", False)},
      "ISteamNetworkingUtils.GetPOPList": {0:("output", "arg1", True)},
      "ISteamNetworkingUtils.SteamNetworkingIPAddr_ToString": {1:("text", "arg2", False)},
      "ISteamNetworkingUtils.SteamNetworkingIdentity_ToString": {1:("text", "arg2", False)},
      "ISteamParties.GetBeaconDetails": {3:("text", "arg4", False)},
      "ISteamParties.GetAvailableBeaconLocations": {0:("output", "arg1", False)},
      "ISteamParties.CreateBeacon": {1:("input", "1", False)},
      "ISteamParties.GetBeaconLocationData": {2:("text", "arg3", False)},
      "ISteamNetworkingFakeUDPPort.SendMessageToFakeIP": {1:("input", "arg2", False)},
    }
    # Capacity pointers use caller-provided input values and return the real
    # required size, including null-buffer size queries and too-small errors.
    counted = {
      "ISteamInventory.GetResultItems": (1,2,"output"),
      "ISteamInventory.SerializeResult": (1,2,"output"),
      "ISteamInventory.GetItemDefinitionIDs": (0,1,"output"),
      "ISteamInventory.GetItemDefinitionProperty": (2,3,"text"),
      "ISteamInventory.GetEligiblePromoItemDefinitionIDs": (1,2,"output"),
      "ISteamInventory.GetResultItemProperty": (3,4,"text"),
      "ISteamVideo.GetOPFStringForApp": (1,2,"text"),
    }
    if key in counted:
        buf,size,direction=counted[key]
        return {buf:(direction,f"arg{buf} && arg{size} ? *arg{size} : 0",False),size:("inout",f"arg{buf}",False)}
    if family == "ISteamInventory" and method == "GetItemsWithPrices":
        size=len(types)-1
        return {i:("output",f"arg{size}",False) for i in range(size)}
    if family == "ISteamUserStats" and method == "GetGlobalStatHistory":
        element=types[1][:-1].strip()
        return {1:("output",f"arg2 / sizeof({element})",True)}
    if family == "ISteamUGC":
        if method in ("GetSubscribedItems","GetDownloadedItems"):return {0:("output","arg1",True)}
        if method == "GetItemInstallInfo":return {2:("text","arg3",False)}
        if method == "GetQueryUGCKeyValueTag":
            return {3:("text","arg4",False)} if len(types)==5 else {3:("text","arg4",False),5:("text","arg6",False)}
        if method == "GetQueryUGCAdditionalPreview":
            return {3:("text","arg4",False)} if len(types)==6 else {3:("text","arg4",False),5:("text","arg6",False)}
    if family == "ISteamMatchmaking" and method == "RequestLobbyList" and len(types)==3:
        return {1:("input","arg2",False)}
    if family in ("ISteamGameServer","ISteamMasterServerUpdater"):
        if method == "BeginAuthSession":return {0:("input","arg1",False)}
        if method == "GetAuthSessionTicket":return {0:("output","arg1",False)}
        if method == "HandleIncomingPacket":return {0:("input","arg1",False)}
        if method == "GetNextOutgoingPacket":return {0:("output","arg1",True)}
        if method == "GSGetSteam2GetEncryptionKeyToSendToNewClient":return {0:("output","arg2",False)}
        if method == "GSSendSteam2UserConnect":return {1:("input","arg2",False),5:("input","arg6",False)}
        if method == "GSSendSteam3UserConnect":return {2:("input","arg3",False)}
        if method == "GSSendUserConnect":return {3:("input","arg4",False)}
        if method in ("SendUserConnectAndAuthenticate","SendUserConnectAndAuthenticate_DEPRECATED"):
            return {2:("input","arg3",False)} if types[0]=="CSteamID" else {1:("input","arg2",False)}
    plan=plans.get(key,{})
    # These reference outputs replaced earlier pointer outputs in the SDK.
    if family=="ISteamNetworkingUtils" and method in ("GetLocalPingLocation","ParsePingLocationString"):
        index=0 if method=="GetLocalPingLocation" else 1
        if types[index].endswith('&'):plan={**plan,index:("reference","",False)}
    if family == "ISteamApps" and method == "GetInstalledDepots":
        index = len(types)-2
        return {index:("output", f"arg{index+1}", True)}
    if family == "ISteamUserStats" and method == "UploadLeaderboardScore":
        index = len(types)-2
        return {index:("input", f"arg{index+1}", False)}
    if family == "ISteamUser" and method.startswith("InitiateGameConnection"):
        # The ancient nine-parameter ABI contains another input blob.
        result={0:("output", "arg1", True)}
        if len(types)==9: result[7]=("input", "arg8", False)
        return result
    if family == "ISteamUser" and method == "DecompressVoice":
        return {0:("input", "arg1", False),2:("output", "arg3", False)}
    if family == "ISteamUser" and method == "GetCompressedVoice": return {0:("output", "arg1", False)}
    if family == "ISteamUser" and method == "GetVoice": return {1:("output", "arg2", False),5:("output", "arg6", False)}
    if family == "ISteamUtils" and method == "FilterText":
        return {3:("text", "arg4", False)} if len(types)==5 else {0:("text", "arg1", False)}
    if family == "ISteamFriends" and method == "SendMsgToFriend" and len(types)==4: return {2:("input", "arg3", False)}
    if family in ["ISteamController", "ISteamInput"]:
        if method == "GetConnectedControllers": return {0:("output", "STEAM_INPUT_MAX_COUNT", True)}
        if method == "GetActiveActionSetLayers": return {1:("output", "STEAM_INPUT_MAX_ACTIVE_LAYERS", True)}
        if method in ["GetDigitalActionOrigins", "GetAnalogActionOrigins"]: return {3:("output", "STEAM_INPUT_MAX_ORIGINS", True)}
    return plan
