#!/usr/bin/env node
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),crypto=require('node:crypto'),os=require('node:os');
const ts=require(process.argv[3]);
const models=require(path.resolve(process.argv[2],'model/ValueAddedModels.js'));
const app=require(path.resolve(process.argv[2],'model/AppModels.js'));
const directory=fs.mkdtempSync(path.join(os.tmpdir(),'vp-account-service-'));
let visible=true,requests=0,constructors=0,finishCalls=0,failFinish=true,writes=0,user='alice',authorizationState='';
const config={apiBaseUrl:'https://api.example.com',accountClientId:'client-id',membershipProductId:'month',membershipProductType:3,coffeeProductId:'coffee'};
const profile=models.cloudProfilePayload(app.createDefaultInputProfile());
const envelope={schemaVersion:1,profileFamily:'input/native-v6',deviceClass:'tablet',variant:profile.id,revision:1,baseRevision:0,
  payloadSha256:crypto.createHash('sha256').update(JSON.stringify(profile)).digest('hex'),payload:profile};
let route=async(url,options)=>{
 const endpoint=new URL(url).pathname;
 if(endpoint==='/v1/capabilities')return {account:true,commerce:true,profiles:true,cloudSaves:false,reason:''};
 if(endpoint==='/v1/auth/begin')return {attemptId:'test-attempt',state:'test-state'};
 if(endpoint==='/v1/auth/exchange')return {token:'session-'+user,userId:user,expiresAt:Date.now()+1800000};
 if(endpoint==='/v1/auth/logout')return {};
 if(endpoint==='/v1/purchases/intent')return {developerPayload:'bound-intent'};
 if(endpoint==='/v1/purchases/verify')return {status:'GRANTED',finishAllowed:true,productId:'month',productType:3,purchaseOrderId:'order',purchaseToken:'provider-token'};
 if(endpoint==='/v1/profiles/tablet')return {profiles:[{variant:profile.id,name:profile.name,revision:1,deviceClass:'tablet'}]};
 if(endpoint==='/v1/profiles/tablet/default')return envelope;
 throw Error(endpoint);
};
const http={RequestMethod:{GET:'GET',POST:'POST',PUT:'PUT'},HttpDataType:{STRING:'string'},createHttp(){requests++;return{
 request:async(url,opts)=>({responseCode:200,result:JSON.stringify(await route(url,opts))}),destroy(){}};}};
const authentication={HuaweiIDProvider:class{constructor(){constructors++;}createAuthorizationWithHuaweiIDRequest(){return {};}},
 AuthenticationController:class{async executeRequest(request){authorizationState=request.state;assert.deepEqual(Array.from(request.scopes),['profile']);assert.deepEqual(Array.from(request.permissions),['serviceauthcode']);return {state:request.state,data:{authorizationCode:'provider-code'}};}}};
const iap={ProductType:{CONSUMABLE:0},PurchaseQueryType:{UNFINISHED:1,ALL:0},queryProducts:async()=>[],
 createPurchase:async()=>({purchaseData:'opaque-provider-receipt'}),
 queryPurchases:async(_,{productType,queryType})=>({purchaseDataList:productType===3&&queryType===1?['opaque-provider-receipt']:[]}),
 finishPurchase:async()=>{finishCalls++;if(failFinish)throw Error('offline');}};
const adapter={OpenMode:{CREATE:fs.constants.O_CREAT,WRITE_ONLY:fs.constants.O_WRONLY,NOFOLLOW:fs.constants.O_NOFOLLOW},
 accessSync:fs.existsSync,mkdirSync:fs.mkdirSync,openSync:(p,f)=>({fd:fs.openSync(p,f,0o600)}),
 closeSync:f=>fs.closeSync(f.fd),writeSync:(fd,b)=>fs.writeSync(fd,Buffer.from(b)),fsyncSync:fs.fsyncSync};
const md={createMd(){const hash=crypto.createHash('sha256');return {update:async({data})=>{hash.update(data);},digest:async()=>({data:new Uint8Array(hash.digest())})};}};
const imports={
 '@kit.AbilityKit':{bundleManager:{BundleFlag:{GET_BUNDLE_INFO_WITH_HAP_MODULE:2,GET_BUNDLE_INFO_WITH_METADATA:32},
 getBundleInfoForSelfSync:()=>({hapModulesInfo:[{name:'entry',metadata:[{name:'client_id',value:'client-id'}]}]})}},
 '@kit.AccountKit':{authentication},'@kit.IAPKit':{iap},'@ohos.net.http':{default:http},
 '@ohos.util':{default:{TextDecoder:{create:()=>({decodeToString:b=>Buffer.from(b).toString()})},
 TextEncoder:class{encodeInto(s){return new TextEncoder().encode(s);}},generateRandomUUID:crypto.randomUUID}},
 '@kit.CryptoArchitectureKit':{cryptoFramework:md},'@kit.CoreFileKit':{fileIo:adapter},
 '../../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:{supportsValueAddedFeatures:()=>visible,deviceType:()=>visible?'tablet':'phone'}},
 '../../model/AppModels':app,'../../model/ValueAddedModels':models,
 '../InputProfileService':{InputProfileService:{getInstance:()=>({hasProfile:()=>true,getProfile:()=>profile,saveProfile:async()=>{writes++;}})}}
};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/valueadded/ValueAddedService.ets'),'utf8');
const js=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
const mod={exports:{}};
vm.runInNewContext(js,{module:mod,exports:mod.exports,require:n=>{if(!imports[n])throw Error(n);return imports[n];},setTimeout,ArrayBuffer,Uint8Array,TextEncoder,console});
const service=mod.exports.ValueAddedService.getInstance();
const context={filesDir:directory,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(config))}};
(async()=>{
 visible=false;await assert.rejects(service.initialize(context),/FEATURE_HIDDEN/);await assert.rejects(service.signIn(),/FEATURE_HIDDEN/);
 assert.equal(requests,0);assert.equal(constructors,0);visible=true;
 await service.initialize(context);await service.signIn();assert.equal(authorizationState,'test-state');assert.equal(service.getUserId(),'alice');
 assert.equal(await service.purchase({id:'month',type:3}),'ACK_PENDING');assert.equal(finishCalls,1);
 failFinish=false;assert.equal(await service.restorePurchases(),'RESTORED');assert.equal(finishCalls,2);
 await service.downloadProfile('default');assert.equal(writes,1);
 const realBackup=service.saveLocalBranch.bind(service);let releaseBackup,backupEntered;
 const entered=new Promise(resolve=>backupEntered=resolve),paused=new Promise(resolve=>releaseBackup=resolve);
 service.saveLocalBranch=async(...args)=>{await realBackup(...args);backupEntered();await paused;};
 const downloading=service.downloadProfile('default');await entered;await service.signOut();releaseBackup();
 await assert.rejects(downloading,/ACCOUNT_CHANGED/);assert.equal(writes,1);assert.equal(service.profileHeads.size,0);
 service.saveLocalBranch=realBackup;user='bob';await service.signIn();
 const originalRoute=route;let releaseList,listEntered;
 const listingStarted=new Promise(resolve=>listEntered=resolve),listPaused=new Promise(resolve=>releaseList=resolve);
 route=async(url,opts)=>{if(new URL(url).pathname==='/v1/profiles/tablet'){listEntered();await listPaused;}return originalRoute(url,opts);};
 const listing=service.listCloudProfiles();await listingStarted;await service.signOut();releaseList();await assert.rejects(listing,/ACCOUNT_CHANGED/);
 route=originalRoute;await service.signIn();
 const originalHash=envelope.payloadSha256;envelope.payloadSha256='0'.repeat(64);
 await assert.rejects(service.downloadProfile('default'),/PROFILE_HASH_MISMATCH/);envelope.payloadSha256=originalHash;
 assert.equal(writes,1);
 console.log('Value-added services: hidden no-network/no-SDK, authorization state, ack retry, account-switch races, profile hash PASS');
})().finally(()=>fs.rmSync(directory,{recursive:true,force:true})).catch(error=>{console.error(error);process.exitCode=1;});
