@ stub AuthzrExtAccessCheck
@ stub AuthzrExtFreeContext
@ stub AuthzrExtFreeResourceManager
@ stub AuthzrExtGetInformationFromContext
@ stub AuthzrExtInitializeCompoundContext
@ stub AuthzrExtInitializeContextFromSid
@ stub AuthzrExtInitializeRemoteResourceManager
@ stub AuthzrExtModifyClaims
@ stub DsAddressToSiteNamesA
@ stub DsAddressToSiteNamesExA
@ stub DsAddressToSiteNamesExW
@ stub DsAddressToSiteNamesW
@ stub DsDeregisterDnsHostRecordsA
@ stub DsDeregisterDnsHostRecordsW
@ stdcall DsEnumerateDomainTrustsA(str long ptr ptr) netapi32.DsEnumerateDomainTrustsA
@ stdcall DsEnumerateDomainTrustsW(wstr long ptr ptr) netapi32.DsEnumerateDomainTrustsW
@ stub DsGetDcCloseW
@ stdcall DsGetDcNameA(str str ptr str long ptr) netapi32.DsGetDcNameA
@ stdcall DsGetDcNameW(wstr wstr ptr wstr long ptr) netapi32.DsGetDcNameW
@ stub DsGetDcNameWithAccountA
@ stub DsGetDcNameWithAccountW
@ stub DsGetDcNextA
@ stub DsGetDcNextW
@ stub DsGetDcOpenA
@ stub DsGetDcOpenW
@ stub DsGetDcSiteCoverageA
@ stub DsGetDcSiteCoverageW
@ stub DsGetForestTrustInformationW
@ stdcall DsGetSiteNameA(str ptr) netapi32.DsGetSiteNameA
@ stdcall DsGetSiteNameW(wstr ptr) netapi32.DsGetSiteNameW
@ stub DsMergeForestTrustInformationW
@ stub DsValidateSubnetNameA
@ stub DsValidateSubnetNameW
@ stub I_DsUpdateReadOnlyServerDnsRecords
@ stub I_NetAccountDeltas
@ stub I_NetAccountSync
@ stub I_NetChainSetClientAttributes2
@ stub I_NetChainSetClientAttributes
@ stub I_NetDatabaseDeltas
@ stub I_NetDatabaseRedo
@ stub I_NetDatabaseSync2
@ stub I_NetDatabaseSync
@ stub I_NetExtendMachinePasswordExpirationTimeout
@ stub I_NetGetDCList
@ stub I_NetGetForestTrustInformation
@ stub I_NetLogonControl2
@ stub I_NetLogonControl
@ stub I_NetLogonGetCapabilities
@ stub I_NetLogonGetDomainInfo
@ stub I_NetLogonSamLogoff
@ stub I_NetLogonSamLogon
@ stub I_NetLogonSamLogonEx
@ stub I_NetLogonSamLogonWithFlags
@ stub I_NetLogonSendToSam
@ stub I_NetLogonUasLogoff
@ stub I_NetLogonUasLogon
@ stub I_NetQuerySecureChannelDCInfo
@ stub I_NetServerAuthenticate2
@ stub I_NetServerAuthenticate3
@ stub I_NetServerAuthenticate
@ stub I_NetServerGetTrustInfo
@ stub I_NetServerPasswordGet
@ stub I_NetServerPasswordSet2
@ stub I_NetServerPasswordSet
@ stub I_NetServerReqChallenge
@ stub I_NetServerTrustPasswordsGet
@ stub I_NetlogonComputeClientDigest
@ stub I_NetlogonComputeClientSignature
@ stub I_NetlogonComputeServerDigest
@ stub I_NetlogonComputeServerSignature
@ stub I_NetlogonGetTrustRid
@ stub I_RpcExtInitializeExtensionPoint
@ stub NetAddServiceAccount
@ stub NetEnumerateServiceAccounts
@ stub NetEnumerateTrustedDomains
@ stdcall NetGetAnyDCName(wstr wstr ptr) netapi32.NetGetAnyDCName
@ stdcall NetGetDCName(wstr wstr ptr) netapi32.NetGetDCName
@ stub NetIsServiceAccount
@ stub NetLogonGetTimeServiceParentDomain
@ stub NetLogonSetServiceBits
@ stub NetQueryServiceAccount
@ stub NetRemoveServiceAccount
@ stub NlBindingAddServerToCache
@ stub NlBindingRemoveServerFromCache
@ stub NlBindingSetAuthInfo
@ stub NlSetDsIsCloningPDC
