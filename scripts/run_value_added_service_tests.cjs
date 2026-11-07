#!/usr/bin/env node
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),crypto=require('node:crypto'),os=require('node:os');
// --source transpiles the current models in memory; the compiled-model-dir interface stays compatible.
const ts=require(process.argv[3]);
const sourceRoot=path.join(__dirname,'../entry/src/main/ets'),modelCache=new Map();
function loadModel(file,fromSource=process.argv[2]==='--source'){
 if(!fromSource)return require(path.resolve(process.argv[2],file+'.js'));
 if(modelCache.has(file))return modelCache.get(file).exports;
 const module={exports:{}};modelCache.set(file,module);
 const source=fs.readFileSync(path.join(sourceRoot,file+'.ets'),'utf8');
 const compiled=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
 vm.runInNewContext(compiled,{module,exports:module.exports,require(id){
  if(id==='@kit.LocalizationKit'&&file==='model/MembershipCampaign')return {intl:{NumberFormat:class{
   constructor(_locale,options){this.currency=options.currency;}format(value){return this.currency+' '+value.toFixed(2);}
  }}};
  assert.ok(id.startsWith('.'),'unexpected model dependency '+id);
  return loadModel(path.posix.normalize(path.posix.join(path.posix.dirname(file),id)),true);
 }},{filename:file+'.ets'});
 return module.exports;
}
const models=loadModel('model/ValueAddedModels');
const app=loadModel('model/AppModels');
const directory=fs.mkdtempSync(path.join(os.tmpdir(),'vp-account-service-'));
const completionWatchdog=setTimeout(()=>{console.error('Value-added service tests did not finish');process.exitCode=1;},15000);
let visible=true,requests=0,constructors=0,finishCalls=0,failFinish=true,writes=0,user='alice',authorizationState='', sdkError=0, wrongState=false;
const identityStores=new WeakMap(),authRequests=[];
const preferences={getPreferences:async(context,name)=>{
 if(!identityStores.has(context))identityStores.set(context,new Map());
 const stores=identityStores.get(context);if(!stores.has(name))stores.set(name,new Map());
 const store=stores.get(name);return {getSync:(k,d)=>store.has(k)?store.get(k):d,put:async(k,v)=>store.set(k,v),flush:async()=>{}};
}};
const config={apiBaseUrl:'https://api.example.com',accountClientId:'client-id',membershipProductId:'month',membershipProductType:3,coffeeProductId:'coffee',membershipOperatorName:'Test publisher'};
const profile=models.cloudProfilePayload(app.createDefaultInputProfile());
const envelope={schemaVersion:1,profileFamily:'input/native-v6',deviceClass:'tablet',variant:profile.id,revision:1,baseRevision:0,
  payloadSha256:crypto.createHash('sha256').update(JSON.stringify(profile)).digest('hex'),payload:profile};
let cloudCalls=0, cloudReply=null, functionSupported=true;
let notificationsAllowed=true,notificationToken='push-test-token',permissionGranted=true,bindCalls=0,unbindCalls=0;
const capabilityOverrides={};
const notificationTimers=new Map();let notificationTimerId=0;
let route=async(url,options)=>{
 const endpoint=new URL(url).pathname;
 if(endpoint==='/v1/capabilities')return {account:true,commerce:true,profiles:true,preferences:true,cloudSaves:false,renewalReminders:true,reason:'',...capabilityOverrides};
 if(endpoint==='/v1/notification-devices/register')return {ready:true};
 if(endpoint==='/v1/notification-devices/revoke')return {};
 if(endpoint==='/v1/entitlement')return {active:false,expiresAt:0,serverTime:Date.now()};
 if(endpoint==='/v1/auth/begin')return {attemptId:'test-attempt',state:'test-state'};
 if(endpoint==='/v1/auth/exchange')return {token:'session-'+user,userId:user,expiresAt:Date.now()+1800000};
 if(endpoint==='/v1/auth/logout')return {};
 if(endpoint==='/v1/purchases/intent'){const body=JSON.parse(options.extraData);return {developerPayload:'bound-intent',
 ...(body.promotionalOfferId&&!omitOfferSignature?{promotionalPurchase:{promotionalOfferId:body.promotionalOfferId,applicationUserName:user,jwsRepresentation:'AAA.BBB.CCC'}}:{})};}
 if(endpoint==='/v1/purchases/verify')return {status:'GRANTED',finishAllowed:true,productId:'month',productType:3,purchaseOrderId:'order',purchaseToken:'provider-token'};
 if(endpoint==='/v1/profiles/tablet')return {profiles:[{variant:profile.id,name:profile.name,revision:1,deviceClass:'tablet'}]};
 if(endpoint==='/v1/profiles/tablet/default')return envelope;
 throw Error(endpoint);
};
const http={RequestMethod:{GET:'GET',POST:'POST',PUT:'PUT'},HttpDataType:{STRING:'string'},createHttp(){requests++;return{
 request:async(url,opts)=>({responseCode:200,result:JSON.stringify(await route(url,opts))}),destroy(){}};}};
const authentication={HuaweiIDProvider:class{constructor(){constructors++;}createLoginWithHuaweiIDRequest(){return {login:true};}createAuthorizationWithHuaweiIDRequest(){return {};}},
 AuthenticationController:class{async executeRequest(request){authRequests.push({...request});if(sdkError)throw {code:sdkError};if(request.login){assert.equal(typeof request.forceLogin,'boolean');return {state:wrongState?'wrong':request.state,data:{openID:'sdk-open-id'}};}
 if(!request.permissions){assert.deepEqual(Array.from(request.scopes),['profile']);return {state:request.state,data:{openID:'sdk-open-id',nickName:'Test nickname',avatarUri:'https://avatar.example.test/a.png'}};}
 authorizationState=request.state;assert.deepEqual(Array.from(request.scopes),['profile']);assert.deepEqual(Array.from(request.permissions),['serviceauthcode']);return {state:request.state,data:{authorizationCode:'provider-code'}};}}};
let sandboxActive=true,managedSubscriptions=0,purchaseCalls=0,promotionalOffers=[],latestPurchase,omitOfferSignature=false,createError=0,autoReceipts=[];const subscriptionQueries=[];
const iap={IAPErrorCode:{USER_CANCELED:1,PRODUCT_OWNED:2},isSandboxActivated:async()=>sandboxActive,PeriodUnit:{MONTH:2},OfferPaymentMode:{PAY_AS_YOU_GO:2},ProductType:{CONSUMABLE:0,NONCONSUMABLE:1,AUTORENEWABLE:2,NONRENEWABLE:3},PurchaseQueryType:{UNFINISHED:1,ALL:0,CURRENT_ENTITLEMENT:2},
 WindowScreenMode:{DIALOG_BOX:0},showManagedSubscriptions:async(_,options)=>{assert.equal(options.windowScreenMode,0);managedSubscriptions++;},
 queryProducts:async(_,{productType,productIds})=>[
  {id:productIds[0],type:productType,name:productType===0?'Coffee':'Membership',localPrice:productType===2?'¥5.00':'CNY 6.00',price:'6',
   currency:'CNY',microPrice:5000000,subscriptionInfo:{periodUnit:2,periodCount:1},promotionalOffers},
  {id:'unexpected',type:productType,name:'Other product',price:'999'}],
 createPurchase:async(_,parameter)=>{
  purchaseCalls++;latestPurchase=parameter;if(createError)throw {code:createError};
  if(parameter.productType===2)assert.equal(Object.hasOwn(parameter,'quantity'),false,'automatic subscription must omit consumable-only quantity');
  return {purchaseData:'opaque-provider-receipt'};
 },
 queryPurchases:async(_,{productType,queryType})=>{subscriptionQueries.push({productType,queryType});
  return {purchaseDataList:productType===2?autoReceipts:productType===3&&queryType===1?['opaque-provider-receipt']:[]};},
 finishPurchase:async()=>{finishCalls++;if(failFinish)throw Error('offline');}};
const adapter={OpenMode:{CREATE:fs.constants.O_CREAT,WRITE_ONLY:fs.constants.O_WRONLY,NOFOLLOW:fs.constants.O_NOFOLLOW},
 accessSync:fs.existsSync,mkdirSync:fs.mkdirSync,openSync:(p,f)=>({fd:fs.openSync(p,f,0o600)}),
 closeSync:f=>fs.closeSync(f.fd),writeSync:(fd,b)=>fs.writeSync(fd,Buffer.from(b)),fsyncSync:fs.fsyncSync};
const md={createMd(){const hash=crypto.createHash('sha256');return {update:async({data})=>{hash.update(data);},digest:async()=>({data:new Uint8Array(hash.digest())})};}};
const imports={
 '@kit.AbilityKit':{bundleManager:{BundleFlag:{GET_BUNDLE_INFO_WITH_HAP_MODULE:2,GET_BUNDLE_INFO_WITH_METADATA:32},
 getBundleInfoForSelfSync:()=>({hapModulesInfo:[{name:'entry',metadata:[{name:'client_id',value:'client-id'}]}]})}},
 '@kit.CloudFoundationKit':{cloudFunction:{call:async params=>{
 cloudCalls++;assert.equal(params.name,'vp-account-sync');assert.equal(params.version,'$latest');
 if(cloudReply)return {result:await cloudReply(params)};
 const data=params.data;assert.equal(data.protocol,1);
 return {result:{protocol:1,status:200,body:JSON.stringify(await route('https://cloud.test'+data.path,{method:data.method,extraData:data.body}))}};
 }}},'@ohos.deviceInfo':{default:{sdkApiVersion:23}},
 '@kit.PushKit':{pushCommon:{AppProfileType:{PROFILE_TYPE_APPLICATION_ACCOUNT:1}},pushService:{getToken:async()=>notificationToken,bindAppProfileId:async()=>{bindCalls++;},unbindAppProfileId:async()=>{unbindCalls++;}}},
 '@kit.NotificationKit':{notificationManager:{isNotificationEnabled:async()=>notificationsAllowed,requestEnableNotification:async()=>{notificationsAllowed=permissionGranted;}}},
 '@kit.BasicServicesKit':{},'@kit.AccountKit':{authentication},'@kit.IAPKit':{iap},'@ohos.net.http':{default:http},'@ohos.data.preferences':{default:preferences},
 '@ohos.util':{default:{TextDecoder:{create:()=>({decodeToString:b=>Buffer.from(b).toString()})},
 TextEncoder:class{encodeInto(s){return new TextEncoder().encode(s);}},generateRandomUUID:crypto.randomUUID}},
 '@kit.CryptoArchitectureKit':{cryptoFramework:md},'@kit.CoreFileKit':{fileIo:adapter},
 '../../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:{supportsValueAddedFeatures:()=>visible,deviceType:()=>visible?'tablet':'phone'}},
 '../../model/AppModels':app,'../../model/AccountSyncModels':loadModel('model/AccountSyncModels'),
  '../../model/ValueAddedModels':models,'../../model/MembershipCommerce':loadModel('model/MembershipCommerce'),
  '../../model/MembershipCampaign':loadModel('model/MembershipCampaign',true),
 '../InputProfileService':{InputProfileService:{getInstance:()=>({hasProfile:()=>true,getProfile:()=>profile,saveProfile:async()=>{writes++;}})}}
};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/valueadded/ValueAddedService.ets'),'utf8');
const js=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
const mod={exports:{}};
vm.runInNewContext(js,{module:mod,exports:mod.exports,require:n=>{if(!imports[n])throw Error(n);return imports[n];},canIUse:()=>functionSupported,
 setTimeout:(f,ms)=>{notificationTimers.set(++notificationTimerId,{f,ms});return notificationTimerId;},clearTimeout:id=>notificationTimers.delete(id),
 ArrayBuffer,Uint8Array,TextEncoder,console});
const service=mod.exports.ValueAddedService.getInstance();
async function purchaseConfirmed(service, product){
 if(!product.purpose)product=(await service.products()).find(p=>p.id===product.id&&p.type===product.type);
 return service.purchase(product,{userId:service.getUserId(),quote:service.quoteKey(product),termsVersion:'2026-10-16',membershipAccepted:true,autoRenewAccepted:product.type===2});
}
const context={filesDir:directory,resourceManager:{getRawFileContent:async name=>Buffer.from(JSON.stringify(
 name==='membership_campaigns.json'?{schema:1,campaigns:[]}:config))}};
async function testLifetimeMembership(){
 const savedRoute=route,savedUser=user;
 const savedIap=Object.fromEntries(['queryProducts','queryPurchases','createPurchase','finishPurchase'].map(name=>[name,iap[name]]));
 const {NONCONSUMABLE:vipType,AUTORENEWABLE:monthlyType,NONRENEWABLE:legacyType,CONSUMABLE:coffeeType}=iap.ProductType;
 const {UNFINISHED,CURRENT_ENTITLEMENT,ALL}=iap.PurchaseQueryType;
 const vipConfig={...config,membershipProductId:'vip_monthly',membershipProductType:monthlyType,
  membershipOneTimeProductId:'vip_monthly_once',membershipOneTimePurchases:false,
  membershipLifetimeProductId:'VIP',membershipLifetimePurchases:true};
 const campaign={schema:1,campaigns:[{productId:'VIP',offerId:'local_28',currency:'CNY',
  originalMicroPrice:68000000,microPrice:28000000,endsAt:'2099-01-01T00:00:00+08:00'}]};
 const makeContext=settings=>({...context,resourceManager:{getRawFileContent:async name=>Buffer.from(JSON.stringify(
  name==='membership_campaigns.json'?campaign:settings))}});
 const officialVip={id:'VIP',type:vipType,name:'VIP',price:'38',localPrice:'CNY 38.00',
  microPrice:38000000,originalMicroPrice:68000000,currency:'CNY'};
 let vipDetails=officialVip,lookupIssue='',coffeeMissing=false,pendingFinish=false;
 const productQueries=[],purchaseQueries=[],intents=[],created=[],finished=[],verified=[],trace=[];
 let queryReceipts=async()=>({purchaseDataList:[]});
 const verdict=(productId,productType,order='vip-order')=>({status:'GRANTED',finishAllowed:true,
  productId,productType,purchaseOrderId:order,purchaseToken:order+'-token'});
 let verifyResponse=()=>verdict('VIP',vipType);
 try {
  iap.queryProducts=async(_,{productType,productIds})=>{
   productQueries.push({productType,productIds:Array.from(productIds)});trace.push('query:'+productType);
   if(productIds[0]==='VIP'){
    assert.equal(productType,vipType,'lifetime lookup must use the official nonconsumable type');
    if(lookupIssue==='missing')return [];
    if(lookupIssue==='error')throw {code:1001860002};
    if(lookupIssue==='invalid')return [{...vipDetails,microPrice:0}];
    return [{...vipDetails}];
   }
   if(productIds[0]==='vip_monthly'){
    assert.equal(productType,monthlyType);
    return [{id:'vip_monthly',type:monthlyType,name:'Monthly',price:'5',localPrice:'CNY 5.00',
     microPrice:5000000,currency:'CNY',subscriptionInfo:{periodUnit:iap.PeriodUnit.MONTH,periodCount:1}}];
   }
   assert.equal(productIds[0],'coffee','retired one-time month must never be advertised');assert.equal(productType,coffeeType);
   return coffeeMissing?[]:[{id:'coffee',type:coffeeType,name:'Coffee',price:'6',localPrice:'CNY 6.00',microPrice:6000000,currency:'CNY'}];
  };
  iap.queryPurchases=async(_,args)=>{
   purchaseQueries.push({productType:args.productType,queryType:args.queryType,continuationToken:args.continuationToken??''});
   return queryReceipts(args);
  };
  iap.createPurchase=async(_,parameter)=>{
   purchaseCalls++;latestPurchase=parameter;created.push({...parameter});trace.push('create:'+parameter.productType);
   return {purchaseData:'opaque-vip-receipt'};
  };
  iap.finishPurchase=async(_,parameter)=>{
   finishCalls++;finished.push({...parameter});trace.push('finish:'+parameter.productType);
   if(pendingFinish&&parameter.purchaseOrderId==='vip-unfinished')throw Error('OFFLINE');
  };
  route=async(url,options)=>{
   const endpoint=new URL(url).pathname;
   if(endpoint==='/v1/purchases/intent'){
    const body=JSON.parse(options.extraData);intents.push(body);trace.push('intent:'+body.productType);
    assert.equal(body.productId,'VIP');assert.equal(body.productType,vipType);
    return {developerPayload:'bound-vip-intent',...(body.promotionalOfferId?{promotionalPurchase:{
     promotionalOfferId:body.promotionalOfferId,applicationUserName:user,jwsRepresentation:'AAA.BBB.CCC'}}:{})};
   }
   if(endpoint==='/v1/purchases/verify'){
    const body=JSON.parse(options.extraData);verified.push(body);trace.push('verify:'+body.productType);
    return verifyResponse(body);
   }
   return savedRoute(url,options);
  };
  const vipService=new mod.exports.ValueAddedService();await vipService.initialize(makeContext(vipConfig));await vipService.signIn();
  productQueries.length=0;
  const products=await vipService.products(),vipProduct=products.find(p=>p.id==='VIP'),monthlyProduct=products.find(p=>p.id==='vip_monthly');
  assert.deepEqual(productQueries,[{productType:monthlyType,productIds:['vip_monthly']},
   {productType:vipType,productIds:['VIP']},{productType:coffeeType,productIds:['coffee']}]);
  assert.equal(vipProduct.type,1);assert.equal(vipProduct.lifetime,true);assert.equal(vipProduct.price,'CNY 38.00');
  assert.equal(vipProduct.microPrice,38000000);assert.equal(vipProduct.promotion,undefined);
  assert.equal(vipProduct.campaignLabel,undefined,'local 28 campaign cannot override the official 38 quote');
  const preview=vipService.previewProducts().find(p=>p.id==='VIP');
  assert.equal(preview.type,1);assert.equal(preview.preview,true);assert.equal(preview.lifetime,true);assert.equal(preview.price,'CNY 28.00');
  const noPreviewCalls={requests,queries:productQueries.length,intents:intents.length,purchaseCalls,finishCalls};
  for(const readOnly of [preview,{...vipProduct,preview:true},{...monthlyProduct,preview:true}]){
   await assert.rejects(purchaseConfirmed(vipService,readOnly),/PRICE_CHANGED/);
  }
  assert.deepEqual({requests,queries:productQueries.length,intents:intents.length,purchaseCalls,finishCalls},noPreviewCalls,
   'a direct preview call, including an otherwise payable official quote, cannot even begin checkout/requery');
  await assert.rejects(purchaseConfirmed(vipService,{...vipProduct,type:legacyType}),/UNKNOWN_PRODUCT/);
  await assert.rejects(purchaseConfirmed(vipService,{...vipProduct,price:'CNY 28.00',microPrice:28000000}),/PRICE_CHANGED/);
  assert.equal(intents.length,0);assert.equal(created.length,0,'neither type-3 VIP nor an unconfirmed local discount reaches the cashier');
  trace.length=0;
  assert.equal(await purchaseConfirmed(vipService,vipProduct),'FINISHED');
  assert.deepEqual(trace,['query:1','intent:1','create:1','verify:1','finish:1']);
  assert.deepEqual(intents[0],{productId:'VIP',productType:1});
  assert.deepEqual(created[0],{productId:'VIP',productType:1,developerPayload:'bound-vip-intent'});
  assert.deepEqual(verified[0],{purchaseData:'opaque-vip-receipt',productType:1});
  assert.deepEqual(finished[0],{productType:1,purchaseToken:'vip-order-token',purchaseOrderId:'vip-order'});
  // Keep the existing official single-offer support, but never synthesize the local 28 offer.
  vipDetails={...officialVip,price:'68',localPrice:'CNY 68.00',microPrice:68000000,
   promotionalOffers:[{offerId:'official_38',microPrice:38000000,localPrice:'CNY 38.00'}]};
  const officialOffer=(await vipService.products()).find(p=>p.id==='VIP');
  assert.equal(officialOffer.price,'CNY 38.00');assert.equal(officialOffer.promotion.id,'official_38');
  assert.equal(officialOffer.promotion.microPrice,38000000);assert.equal(officialOffer.campaignLabel,undefined);
  assert.equal(await purchaseConfirmed(vipService,officialOffer),'FINISHED');
  assert.equal(intents.at(-1).promotionalOfferId,'official_38');assert.equal(created.at(-1).promotionalOfferId,'official_38');
  assert.equal(created.at(-1).jwsRepresentation,'AAA.BBB.CCC');vipDetails=officialVip;
  const beforeInvalidFinish=finishCalls;
  for(const invalid of [{productId:'unknown'},{productType:legacyType},{productId:'coffee',productType:coffeeType},
   {productId:'vip_monthly',productType:monthlyType},{purchaseToken:''},{purchaseOrderId:''}]){
   verifyResponse=()=>({...verdict('VIP',vipType),...invalid});
   assert.equal(await purchaseConfirmed(vipService,vipProduct),'VERIFICATION_PENDING');
   assert.equal(vipService.getPurchaseRecoveryStatus(),'INVALID_ORDER');assert.equal(finishCalls,beforeInvalidFinish);
  }
  verifyResponse=()=>({status:'ALREADY_FINISHED',finishAllowed:false});
  assert.equal(await purchaseConfirmed(vipService,vipProduct),'ALREADY_FINISHED');assert.equal(finishCalls,beforeInvalidFinish);
  for(const [mode,code] of [['missing','PRODUCT_MISSING'],['error','1001860002'],['invalid','INVALID_PRODUCT_PRICE']]){
   lookupIssue=mode;coffeeMissing=true;await vipService.products();
   assert.deepEqual(JSON.parse(JSON.stringify(vipService.getMembershipProductIssues())),[{id:'VIP',type:1,code,lifetime:true}]);
   const issues=vipService.getMembershipProductIssues();issues[0].lifetime=false;
   assert.equal(vipService.getMembershipProductIssues()[0].lifetime,true,'issue snapshots preserve and isolate lifetime metadata');
  }
  lookupIssue='';coffeeMissing=false;await vipService.products();assert.equal(vipService.getMembershipProductIssues().length,0);
  assert.throws(()=>models.validateValueAddedConfig({...vipConfig,membershipProductType:vipType}),/NOT_CONFIGURED/);
  console.log('Lifetime VIP: official queries 2/1/0, preview type1 nonpayable, type3 rejected, official 38 vs local 28, signed official offer, intent/create/verify/finish type1 and invalid verdicts PASS');

  const retired=new mod.exports.ValueAddedService();
  await retired.initialize(makeContext({...vipConfig,membershipLifetimePurchases:false}));await retired.signIn();
  productQueries.length=0;await retired.products();
  assert.deepEqual(productQueries.map(q=>q.productIds[0]),['vip_monthly','coffee']);
  assert.equal(retired.previewProducts().length,0);assert.equal(retired.getMembershipProductIssues().length,0);
  const beforeRetiredCheckout=purchaseCalls;
  await assert.rejects(purchaseConfirmed(retired,vipProduct),/MEMBERSHIP_NOT_OPEN/);
  await assert.rejects(retired.purchase({id:'vip_monthly_once',type:legacyType,purpose:'membership',price:'CNY 5.00'}),/MEMBERSHIP_NOT_OPEN/);
  assert.equal(purchaseCalls,beforeRetiredCheckout);
  const receiptTypes=new Map([['monthly-unfinished',monthlyType],['monthly-current',monthlyType],
   ['legacy-unfinished',legacyType],['legacy-history',legacyType],['vip-unfinished',vipType],['vip-second',vipType],['vip-current',vipType]]);
  const acknowledged=new Set();
  verifyResponse=body=>{
   const type=receiptTypes.get(body.purchaseData);assert.ok(type!==undefined);assert.equal(body.productType,type);
   if(acknowledged.has(body.purchaseData))return {status:'ALREADY_FINISHED',finishAllowed:false};
   return verdict(type===vipType?'VIP':type===monthlyType?'vip_monthly':'vip_monthly_once',type,body.purchaseData);
  };
  const finishBeforeRecovery=iap.finishPurchase;
  iap.finishPurchase=async(ctx,parameter)=>{await finishBeforeRecovery(ctx,parameter);acknowledged.add(parameter.purchaseOrderId);};
  queryReceipts=async({productType,queryType,continuationToken})=>{
   if(productType===vipType&&queryType===UNFINISHED)return continuationToken?
    {purchaseDataList:['vip-second']}:{purchaseDataList:['vip-unfinished'],continuationToken:'vip-page-2'};
   const receipt=productType===monthlyType?(queryType===UNFINISHED?'monthly-unfinished':'monthly-current'):
    productType===legacyType?(queryType===UNFINISHED?'legacy-unfinished':'legacy-history'):'vip-current';
   return {purchaseDataList:[receipt]};
  };
  const expectedQueries=[{productType:2,queryType:UNFINISHED,continuationToken:''},{productType:2,queryType:CURRENT_ENTITLEMENT,continuationToken:''},
   {productType:3,queryType:UNFINISHED,continuationToken:''},{productType:3,queryType:ALL,continuationToken:''},
   {productType:1,queryType:UNFINISHED,continuationToken:''},{productType:1,queryType:UNFINISHED,continuationToken:'vip-page-2'},
   {productType:1,queryType:CURRENT_ENTITLEMENT,continuationToken:''}];
  purchaseQueries.length=0;pendingFinish=true;const finishBeforePending=finishCalls;
  assert.deepEqual(await Promise.all([retired.restorePurchases(),retired.restorePurchases()]),['ACK_PENDING','ACK_PENDING']);
  assert.deepEqual(purchaseQueries,expectedQueries,'disabled sale switches do not skip historical restoration, and concurrent restores coalesce');
  assert.equal(finishCalls-finishBeforePending,7);assert.equal(acknowledged.size,6);assert.ok(retired.getPurchaseRecoveryCheckedAt()>0);
  pendingFinish=false;purchaseQueries.length=0;const finishBeforeRetry=finishCalls;
  assert.equal(await retired.restorePurchases(),'RESTORED');assert.deepEqual(purchaseQueries,expectedQueries);
  assert.equal(finishCalls-finishBeforeRetry,1,'only the unacknowledged order finishes again');assert.equal(acknowledged.size,7);
  assert.equal(purchaseCalls,beforeRetiredCheckout,'recovery never creates a payment');assert.equal(retired.busy,false);assert.equal(retired.restoring,null);
  queryReceipts=async()=>({purchaseDataList:[]});
  const legacyMain=new mod.exports.ValueAddedService();
  await legacyMain.initialize(makeContext({...vipConfig,membershipProductId:'legacy-main',membershipProductType:legacyType,
   membershipLifetimePurchases:false}));await legacyMain.signIn();purchaseQueries.length=0;
  assert.equal(await legacyMain.restorePurchases(),'RESTORED');
  assert.deepEqual(purchaseQueries,[{productType:3,queryType:UNFINISHED,continuationToken:''},{productType:3,queryType:ALL,continuationToken:''},
   {productType:1,queryType:UNFINISHED,continuationToken:''},{productType:1,queryType:CURRENT_ENTITLEMENT,continuationToken:''}],
   'a primary type3 plus a historical type3 ID is queried only once per query mode');
  queryReceipts=async({productType,queryType})=>({purchaseDataList:[],
   ...(productType===vipType&&queryType===UNFINISHED?{continuationToken:'loop'}:{})});purchaseQueries.length=0;
  await assert.rejects(retired.restorePurchases(),/INVALID_PAGINATION/);
  assert.equal(purchaseQueries.filter(q=>q.productType===vipType).length,2);assert.equal(retired.busy,false);assert.equal(retired.restoring,null);
  let enterQuery,releaseQuery;const queryEntered=new Promise(resolve=>enterQuery=resolve),queryWait=new Promise(resolve=>releaseQuery=resolve);
  queryReceipts=async({productType,queryType})=>{
   if(productType===vipType&&queryType===UNFINISHED){enterQuery();await queryWait;return {purchaseDataList:['old-account-vip']};}
   return {purchaseDataList:[]};
  };
  const beforeSwitchVerify=verified.length,beforeSwitchFinish=finishCalls;
  const oldAccountRestore=retired.restorePurchases();await queryEntered;await retired.signOut();releaseQuery();
  await assert.rejects(oldAccountRestore,/ACCOUNT_CHANGED/);
  assert.equal(verified.length,beforeSwitchVerify);assert.equal(finishCalls,beforeSwitchFinish);
  assert.equal(retired.getUserId(),'');assert.equal(retired.getPurchaseRecoveryStatus(),'');assert.equal(retired.busy,false);assert.equal(retired.restoring,null);
  queryReceipts=async()=>({purchaseDataList:[]});user='bob';await retired.signIn();assert.equal(retired.getUserId(),'bob');await retired.signOut();
  console.log('Lifetime recovery: disabled VIP/legacy sale, type1 unfinished/current pagination, type2 current, type3 ALL/dedup, coalesced restore, ACK_PENDING retry, duplicate cursor and account switch PASS');
 } finally {
  route=savedRoute;user=savedUser;Object.assign(iap,savedIap);
 }
}
(async()=>{
 visible=false;await assert.rejects(service.initialize(context),/FEATURE_HIDDEN/);await assert.rejects(service.signIn(),/FEATURE_HIDDEN/);
 assert.equal(requests,0);assert.equal(constructors,0);visible=true;
 const missing=new mod.exports.ValueAddedService();
 await missing.initialize({...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify({...config,accountClientId:''}))}});
 assert.equal(missing.accountAvailability(),'configuration_missing');assert.equal(requests,0);
 const mismatch=new mod.exports.ValueAddedService();
 await mismatch.initialize({...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify({...config,accountClientId:'other-client'}))}});
 assert.equal(mismatch.accountAvailability(),'identity_mismatch');assert.equal(requests,0);
 const availabilityRoute=route;
 route=async()=>({account:false,commerce:false,profiles:false,cloudSaves:false,renewalReminders:true,reason:'NOT_CONFIGURED'});
 const closed=new mod.exports.ValueAddedService();await closed.initialize(context);assert.equal(closed.accountAvailability(),'service_closed');
 route=async()=>{throw Error('offline');};const unreachable=new mod.exports.ValueAddedService();
 await unreachable.initialize(context);assert.equal(unreachable.accountAvailability(),'unreachable');
 // Official SDK login remains available without an API URL, backend capabilities, or matching cloud client ID.
 const beforeSdkLogin=requests;
 await missing.signIn();assert.equal(missing.isHuaweiSignedIn(),true);assert.equal(missing.getUserId(),'');
 assert.equal(missing.getHuaweiProfile().nickname,'Test nickname');assert.equal(missing.getHuaweiProfile().avatarUri,'https://avatar.example.test/a.png');
 assert.equal(requests,beforeSdkLogin,'independent login makes no business HTTP request');
 await assert.rejects(missing.entitlement(),/NOT_CONFIGURED/);
 await missing.signOut();assert.equal(missing.isHuaweiSignedIn(),false);
 assert.equal(missing.getHuaweiProfile().avatarUri,'');
 sdkError=1001500001;await assert.rejects(missing.signIn(),/HUAWEI_LOGIN_1001500001/);
 assert.equal(missing.isHuaweiSignedIn(),false);sdkError=0;
 wrongState=true;await assert.rejects(missing.signIn(),/AUTH_STATE_MISMATCH/);wrongState=false;
 await unreachable.signIn();assert.equal(unreachable.isHuaweiSignedIn(),true);assert.equal(unreachable.getUserId(),'');
 route=availabilityRoute;
 await service.initialize(context);await service.signIn();assert.equal(service.accountAvailability(),'ready');assert.equal(authorizationState,'test-state');assert.equal(service.getUserId(),'alice');
 const storeProducts=await service.products();
 assert.deepEqual(Array.from(storeProducts,p=>({id:p.id,purpose:p.purpose,price:p.price})),[
  {id:'month',purpose:'membership',price:'CNY 6.00'},{id:'coffee',purpose:'coffee',price:'CNY 6.00'}]);
 const membershipProduct=storeProducts.find(p=>p.purpose==='membership');
 const accepted={userId:'alice',quote:service.quoteKey(membershipProduct),termsVersion:'2026-10-16',membershipAccepted:true,autoRenewAccepted:false};
 const beforeConsent=purchaseCalls;
 await assert.rejects(service.purchase(membershipProduct),/MEMBERSHIP_CONSENT_REQUIRED/);
 for(const override of [{membershipAccepted:false},{userId:'bob'},{quote:'old quote'},{termsVersion:'old'}]) {
  await assert.rejects(service.purchase(membershipProduct,{...accepted,...override}),/MEMBERSHIP_CONSENT_REQUIRED/);
 }
 assert.equal(purchaseCalls,beforeConsent,'missing/account/quote/revision consent never reaches the cashier');
 assert.equal(await purchaseConfirmed(service,{id:'month',type:3}),'ACK_PENDING');assert.equal(finishCalls,1);
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

 const preferencesPayload={schemaVersion:1,themeMode:'system',uiSkin:'classic',consolePalette:'violet',consoleCardLayout:'focus3d',
  viewMode:'grid',useGlassEffect:true,desktopInputProfileId:'default'};
 let preferenceRevision=1;
 const preferenceEnvelope=()=>({schemaVersion:1,deviceClass:'tablet',revision:preferenceRevision,
  payloadSha256:crypto.createHash('sha256').update(JSON.stringify(preferencesPayload)).digest('hex'),payload:preferencesPayload});
 route=async(url,opts)=>{
  if(new URL(url).pathname==='/v1/settings/tablet'){
   if(opts.method==='PUT'){
    const body=JSON.parse(opts.extraData);assert.equal(body.baseRevision,1);assert.deepEqual(body.payload,preferencesPayload);
    preferenceRevision=2;return {status:'SAVED',current:preferenceEnvelope()};
   }
   return {current:preferenceEnvelope()};
  }
  return originalRoute(url,opts);
 };
 assert.equal((await service.readPreferences()).revision,1);
 assert.equal((await service.uploadPreferences(preferencesPayload,1)).current.revision,2);
 preferencesPayload.absolutePath='/private/wine';
 await assert.rejects(service.readPreferences(),/INVALID_PREFERENCES/);delete preferencesPayload.absolutePath;
 let releaseSettings,settingsEntered;
 const settingsStarted=new Promise(resolve=>settingsEntered=resolve),settingsPaused=new Promise(resolve=>releaseSettings=resolve);
 const preferenceRoute=route;
 route=async(url,opts)=>{if(new URL(url).pathname==='/v1/settings/tablet'){settingsEntered();await settingsPaused;}return preferenceRoute(url,opts);};
 const reading=service.readPreferences();await settingsStarted;await service.signOut();releaseSettings();await assert.rejects(reading,/ACCOUNT_CHANGED/);

 route=originalRoute;user='alice';
 const fnConfig={...config,apiBaseUrl:'',cloudFunctionName:'vp-account-sync',cloudFunctionVersion:'$latest'};
 const fnContext={resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(fnConfig))},filesDir:directory};
 const fnService=new mod.exports.ValueAddedService();
 const beforeHttp=requests;
 await fnService.initialize(fnContext);await fnService.signIn();
 assert.equal(fnService.accountAvailability(),'ready');assert.equal(fnService.getUserId(),'alice');
 assert.equal(requests,beforeHttp);assert.ok(cloudCalls>=3);
 cloudReply=async()=>({protocol:1,status:401,body:'{"code":"SIGN_IN_REQUIRED"}'});
 await assert.rejects(fnService.entitlement(),/SIGN_IN_REQUIRED/);assert.equal(fnService.getUserId(),'');
 cloudReply=async()=>({probe:'wrong-deployed-code'});
 const wrongFunction=new mod.exports.ValueAddedService();await wrongFunction.initialize(fnContext);
 assert.equal(wrongFunction.accountAvailability(),'unreachable');
 cloudReply=null;functionSupported=false;
 const unsupported=new mod.exports.ValueAddedService();await unsupported.initialize(fnContext);
 assert.equal(unsupported.accountAvailability(),'unreachable');
 fnConfig.apiBaseUrl=config.apiBaseUrl;
 const fallback=new mod.exports.ValueAddedService();await fallback.initialize(fnContext);
 assert.equal(fallback.accountAvailability(),'ready');assert.ok(requests>beforeHttp);
 functionSupported=true;
 // Late function responses after sign-out cannot restore the previous account.
 await fnService.initialize(fnContext);await fnService.signIn();
 let resolveCloud,enteredCloud;
 const cloudEntered=new Promise(resolve=>enteredCloud=resolve);
 cloudReply=async()=>{enteredCloud();return await new Promise(resolve=>resolveCloud=resolve);};
 const late=fnService.entitlement();await cloudEntered;
 fnService.clearSession();resolveCloud({protocol:1,status:200,body:'{"active":true,"expiresAt":9,"serverTime":1}'});
 await assert.rejects(late,/ACCOUNT_CHANGED/);cloudReply=null;
 console.log('Cloud transport: SDK login session, no HTTP replay, 401, malformed response, capability fallback and stale response PASS');
 const autoService=new mod.exports.ValueAddedService();
 await autoService.initialize({...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify({...config,membershipProductType:2}))}});
 await autoService.signIn();subscriptionQueries.length=0;
 const autoProduct=(await autoService.products()).find(p=>p.type===2);
 const officialQuery=iap.queryProducts;
 const onceService=new mod.exports.ValueAddedService();
 await onceService.initialize({...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(
  {...config,membershipProductType:2,membershipOneTimeProductId:'once'}))}});
 iap.queryProducts=async(ctx,args)=>args.productIds[0]==='once'?[]:officialQuery(ctx,args);
 assert.equal((await onceService.products()).filter(p=>p.purpose==='membership').length,1);
 assert.deepEqual(JSON.parse(JSON.stringify(onceService.getMembershipProductIssues())),[{id:'once',type:3,code:'PRODUCT_MISSING',lifetime:false}]);
 iap.queryProducts=async(ctx,args)=>{if(args.productIds[0]==='once')throw {code:12345};return officialQuery(ctx,args);};
 assert.equal((await onceService.products()).filter(p=>p.purpose==='membership').length,1);
 assert.equal(onceService.getMembershipProductIssues()[0].code,'12345');
 iap.queryProducts=officialQuery;
 assert.equal((await onceService.products()).filter(p=>p.purpose==='membership').length,2);
 assert.equal(onceService.getMembershipProductIssues().length,0,'retry clears a recovered SKU issue');
 const retiredOnceService=new mod.exports.ValueAddedService();
 const retiredConfig={...config,membershipProductType:2,membershipOneTimeProductId:'once',membershipOneTimePurchases:false};
 const beforeRetiredRoute=route;
 route=async(url,opts)=>new URL(url).pathname==='/v1/purchases/verify'?
  {status:'GRANTED',finishAllowed:true,productId:'once',productType:3,purchaseOrderId:'historical-once',purchaseToken:'historical-once-token'}:
  beforeRetiredRoute(url,opts);
 await retiredOnceService.initialize({...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(retiredConfig))}});
 await retiredOnceService.signIn();
 const queried=[];
 iap.queryProducts=async(ctx,args)=>{queried.push(...args.productIds);return officialQuery(ctx,args);};
 assert.equal((await retiredOnceService.products()).filter(p=>p.purpose==='membership').length,1);
 assert.equal(queried.includes('once'),false,'retired SKU is not queried or advertised for sale');
 assert.equal(retiredOnceService.getMembershipProductIssues().length,0);
 const callsBeforeRetiredPurchase=purchaseCalls;
 await assert.rejects(retiredOnceService.purchase({id:'once',type:3,purpose:'membership',price:'¥5.00'}),/MEMBERSHIP_NOT_OPEN/);
 assert.equal(purchaseCalls,callsBeforeRetiredPurchase);
 subscriptionQueries.length=0;
 await retiredOnceService.restorePurchases();
 assert.ok(subscriptionQueries.some(q=>q.productType===3),'retired SKU still participates in historical order recovery');
 iap.queryProducts=officialQuery;subscriptionQueries.length=0;route=beforeRetiredRoute;
 assert.throws(()=>models.validateValueAddedConfig({...retiredConfig,membershipOneTimePurchases:'false'}),/NOT_CONFIGURED/);
 const notificationManager=imports['@kit.NotificationKit'].notificationManager;
 const originalEnabled=notificationManager.isNotificationEnabled;
 notificationManager.isNotificationEnabled=async()=>false;
 const callsBeforeReminderFailure=purchaseCalls;
 await assert.rejects(purchaseConfirmed(autoService,autoProduct),/RENEWAL_NOTIFICATION_REQUIRED/);
 assert.equal(purchaseCalls,callsBeforeReminderFailure,'disabled notifications cannot reach the cashier');
 notificationManager.isNotificationEnabled=originalEnabled;
 capabilityOverrides.renewalReminders=false;
 await assert.rejects(purchaseConfirmed(autoService,autoProduct),/REMINDERS_UNAVAILABLE/);
 assert.equal(purchaseCalls,callsBeforeReminderFailure,'unavailable reminder backend cannot reach the cashier');
 capabilityOverrides.renewalReminders=true;
 // Exercise the service's second payment gate, in addition to the component gate.
 capabilityOverrides.renewalReminders=false;capabilityOverrides.sandboxCheckout=true;
 await assert.rejects(purchaseConfirmed(autoService,autoProduct),/REMINDERS_UNAVAILABLE/);
 capabilityOverrides.sandboxCommerce=true;sandboxActive=false;
 await assert.rejects(purchaseConfirmed(autoService,autoProduct),/SANDBOX_REQUIRED/);
 assert.equal(purchaseCalls,callsBeforeReminderFailure,'test flags cannot replace official sandbox activation');
 sandboxActive=true;createError=iap.IAPErrorCode.USER_CANCELED;
 assert.equal(await purchaseConfirmed(autoService,autoProduct),'CANCELLED');
 assert.equal(purchaseCalls,callsBeforeReminderFailure+1,'official sandbox can exercise the cashier before reminder deployment');
 createError=0;delete capabilityOverrides.sandboxCommerce;delete capabilityOverrides.sandboxCheckout;
 capabilityOverrides.renewalReminders=true;
 await assert.rejects(autoService.purchase(autoProduct,{userId:autoService.getUserId(),quote:autoService.quoteKey(autoProduct),
  termsVersion:'2026-10-16',membershipAccepted:true,autoRenewAccepted:false}),/MEMBERSHIP_CONSENT_REQUIRED/);
 const autoRoute=route, beforeAutoFinish=finishCalls;
 route=async(url,opts)=>{
  if(new URL(url).pathname==='/v1/purchases/verify'){
   assert.equal(JSON.parse(opts.extraData).productType,2);
   return {status:'GRANTED',finishAllowed:true,productId:'month',productType:2,purchaseOrderId:'auto-order',purchaseToken:'provider-auto-token'};
  }
  return autoRoute(url,opts);
 };
 assert.equal(await purchaseConfirmed(autoService,{id:'month',type:2}),'FINISHED');assert.equal(finishCalls,beforeAutoFinish+1);
 route=autoRoute;
 assert.equal(await autoService.restorePurchases(),'RESTORED');
 assert.deepEqual(subscriptionQueries,[{productType:2,queryType:1},{productType:2,queryType:2}]);
 await autoService.manageSubscriptions();assert.equal(managedSubscriptions,1);
 const beforeActiveCreate=purchaseCalls;
 for(const autoRenew of [true,false]){
  route=async(url,opts)=>new URL(url).pathname==='/v1/entitlement'?
   {active:true,refreshRequired:false,autoRenew,expiresAt:Date.now()+10000,serverTime:Date.now()}:autoRoute(url,opts);
  assert.equal(await purchaseConfirmed(autoService,{id:'month',type:2}),'ALREADY_ACTIVE');
  assert.equal(purchaseCalls,beforeActiveCreate,'cancelled renewal must not buy the already-paid current term again');
 }
 route=autoRoute;

 const beforeRecoveryPurchase=purchaseCalls;let notices=0;
 const listener=()=>notices++;autoService.subscribeEntitlement(listener);
 autoReceipts=['official-signed-group-fixture'];
 route=async(url,opts)=>new URL(url).pathname==='/v1/purchases/verify'?
  {status:'ALREADY_FINISHED',finishAllowed:false}:autoRoute(url,opts);
 await autoService.signIn();assert.equal(autoService.getPurchaseRecoveryStatus(),'RESTORED');
 assert.equal(purchaseCalls,beforeRecoveryPurchase);assert.equal(notices,1);
 route=async(url,opts)=>{if(new URL(url).pathname==='/v1/purchases/verify')throw Error('PROVIDER_UNAVAILABLE');return autoRoute(url,opts);};
 await autoService.signIn();assert.equal(autoService.getUserId(),'alice');assert.equal(autoService.getPurchaseRecoveryStatus(),'PROVIDER_UNAVAILABLE');
 assert.equal(purchaseCalls,beforeRecoveryPurchase);
 assert.equal(await purchaseConfirmed(autoService,{id:'month',type:2}),'VERIFICATION_PENDING');assert.equal(autoService.getPurchaseRecoveryStatus(),'PROVIDER_UNAVAILABLE');
 route=async(url,opts)=>new URL(url).pathname==='/v1/purchases/verify'?{status:'ALREADY_FINISHED',finishAllowed:false}:autoRoute(url,opts);
 await autoService.restorePurchases();assert.equal(autoService.getPurchaseRecoveryStatus(),'RESTORED');
 createError=1;assert.equal(await purchaseConfirmed(autoService,{id:'month',type:2}),'CANCELLED');createError=2;
 const beforeOwnedFinish=finishCalls;assert.equal(await purchaseConfirmed(autoService,{id:'month',type:2}),'RESTORED');assert.equal(finishCalls,beforeOwnedFinish);
 createError=0;autoReceipts=[];route=autoRoute;autoService.unsubscribeEntitlement(listener);
 console.log('Membership recovery: automatic login, backend outage without logout, paid order retry, cancellation, owned order reconciliation PASS');
 const offeredService=new mod.exports.ValueAddedService();
 await offeredService.initialize({...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(
  {...config,membershipProductType:2,membershipPromotionalOfferId:'vip_2026_year_end'}))}});
 // Prices remain readable while logged out and commerce is gated; this never grants purchase permission.
 offeredService.capabilities.commerce=false;capabilityOverrides.commerce=false;
 promotionalOffers=[{offerId:'vip_2026_year_end',paymentMode:2,periodUnit:2,periodCount:2,microPrice:2000000,localPrice:'¥2.00'}];
 const offerProduct=(await offeredService.products()).find(p=>p.type===2);
 assert.equal(offerProduct.price,'¥2.00');assert.equal(offerProduct.regularPrice,'¥5.00');
 assert.match(offerProduct.priceDescription,/前 2 个月/);assert.match(offerProduct.priceDescription,/之后 ¥5.00／月/);
 await assert.rejects(purchaseConfirmed(offeredService,offerProduct),/NOT_CONFIGURED/);
 capabilityOverrides.commerce=true;offeredService.capabilities.commerce=true;await offeredService.signIn();
 const beforeOfferRoute=route;
 route=async(url,options)=>new URL(url).pathname==='/v1/purchases/verify'?
  {status:'GRANTED',finishAllowed:true,productId:'month',productType:2,purchaseOrderId:'offer-order',purchaseToken:'offer-token'}:
  beforeOfferRoute(url,options);
 await purchaseConfirmed(offeredService,offerProduct);
 assert.equal(latestPurchase.promotionalOfferId,'vip_2026_year_end');assert.equal(latestPurchase.applicationUserName,'alice');
 assert.equal(latestPurchase.jwsRepresentation,'AAA.BBB.CCC');assert.equal(Object.hasOwn(latestPurchase,'quantity'),false);
 const paidCalls=purchaseCalls;omitOfferSignature=true;
 await assert.rejects(purchaseConfirmed(offeredService,offerProduct),/INVALID_OFFER/);assert.equal(purchaseCalls,paidCalls);omitOfferSignature=false;
 promotionalOffers[0].periodCount=3;
 await assert.rejects(purchaseConfirmed(offeredService,offerProduct),/PRICE_CHANGED/);assert.equal(purchaseCalls,paidCalls);
 promotionalOffers[0].periodCount=2;promotionalOffers[0].microPrice=3000000;
 await assert.rejects(purchaseConfirmed(offeredService,offerProduct),/PRICE_CHANGED/);assert.equal(purchaseCalls,paidCalls);
 promotionalOffers[0].paymentMode=3;
 await assert.rejects(offeredService.products(),/INVALID_OFFER/);assert.equal(purchaseCalls,paidCalls);
 promotionalOffers=[];
 await assert.rejects(purchaseConfirmed(offeredService,offerProduct),/PRICE_CHANGED/);assert.equal(purchaseCalls,paidCalls);
 const fullPrice=(await offeredService.products()).find(p=>p.type===2);assert.equal(fullPrice.price,'¥5.00');assert.equal(fullPrice.promotion,undefined);
 await purchaseConfirmed(offeredService,fullPrice);assert.equal(Object.hasOwn(latestPurchase,'promotionalOfferId'),false);
 route=beforeOfferRoute;
 const sandboxRoute=route;
 route=async(url,options)=>new URL(url).pathname==='/v1/capabilities'?
  {account:true,commerce:true,sandboxCommerce:true,profiles:false,preferences:false,cloudSaves:false,renewalReminders:true,reason:'IAP_NOT_CONFIGURED'}:
  new URL(url).pathname==='/v1/purchases/verify'?
   {status:'GRANTED',finishAllowed:true,productId:'month',productType:2,purchaseOrderId:'sandbox-order',purchaseToken:'sandbox-token'}:
   sandboxRoute(url,options);
 const sandboxContext={...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify({...config,membershipProductType:2}))}};
 const sandboxService=new mod.exports.ValueAddedService();await sandboxService.initialize(sandboxContext);await sandboxService.signIn();
 assert.equal(sandboxService.getCapabilities().commerce,true);assert.equal(sandboxService.getCapabilities().preferences,false);
 await purchaseConfirmed(sandboxService,(await sandboxService.products()).find(p=>p.type===2));
 const sandboxPurchaseCalls=purchaseCalls,sandboxFinishCalls=finishCalls,queryCount=subscriptionQueries.length;
 sandboxActive=false;
 await assert.rejects(purchaseConfirmed(sandboxService,{id:'month',type:2}),/SANDBOX_REQUIRED/);
 await assert.rejects(sandboxService.restorePurchases(),/SANDBOX_REQUIRED/);
 assert.equal(purchaseCalls,sandboxPurchaseCalls);assert.equal(finishCalls,sandboxFinishCalls);assert.equal(subscriptionQueries.length,queryCount);
 const notTester=new mod.exports.ValueAddedService();await notTester.initialize(sandboxContext);assert.equal(notTester.getCapabilities().commerce,false);
 sandboxActive=true;route=sandboxRoute;
 console.log('Sandbox checkout: official tester check, recheck before purchase/restore, no normal-mode purchase or paid sync unlock PASS');
 console.log('Saved Huawei offer: official ¥2/¥5 and cycle text, display while gated, signed checkout, stale/missing/signature/mode safeguards PASS');
 console.log('Automatic subscription: SDK-compatible purchase parameters, current entitlement plus unfinished orders, official management page PASS');

 // A new process keeps only the user's resume intent. Every identity/session/entitlement is verified again.
 const restartContext={...context,resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify({...config,membershipProductType:2}))}};
 const original=new mod.exports.ValueAddedService();await original.initialize(restartContext);await original.signIn();
 const ordersBeforeRestart=purchaseCalls,authBefore=authRequests.length;
 const restarted=new mod.exports.ValueAddedService();await Promise.all([restarted.initialize(restartContext),restarted.initialize(restartContext)]);
 assert.equal(restarted.getUserId(),user);assert.equal(restarted.getHuaweiProfile().nickname,'Test nickname');
 assert.equal(authRequests.length-authBefore,3,'concurrent startup performs one identity/profile/code exchange');
 assert.ok(authRequests.slice(authBefore).every(r=>r.forceLogin===false||r.forceAuthorization===false),'restart never opens login/consent UI');
 assert.equal(purchaseCalls,ordersBeforeRestart,'restoration cannot create a new payment');
 const persisted=identityStores.get(restartContext).get('account_identity');
 assert.deepEqual([...persisted.keys()].sort(),['notification_installation_v1','resume_login_v1']);
 assert.match(persisted.get('notification_installation_v1'),/^[a-f0-9-]{36}$/);
 assert.equal(persisted.get('resume_login_v1'),'true');
 restarted.session.expiresAt=Date.now()-1;await restarted.resumeAccount();assert.equal(restarted.getUserId(),user);
 await restarted.signOut();const afterLogout=authRequests.length;
 const signedOutProcess=new mod.exports.ValueAddedService();await signedOutProcess.initialize(restartContext);await signedOutProcess.resumeAccount();
 assert.equal(signedOutProcess.getUserId(),'');assert.equal(authRequests.length,afterLogout,'explicit logout survives restart');
 const retryIdentity=new mod.exports.ValueAddedService();await retryIdentity.initialize({...context});await retryIdentity.signIn();
 retryIdentity.session.expiresAt=Date.now()-1;sdkError=1001502005;await retryIdentity.resumeAccount();sdkError=0;
 assert.equal(retryIdentity.getUserId(),'');assert.equal(retryIdentity.hasRememberedAccount(),true);
 await retryIdentity.resumeAccount();assert.equal(retryIdentity.getUserId(),user);
 // Failure on an obsolete receipt must not skip the later current receipt or duplicate restore jobs.
 const restoreService=new mod.exports.ValueAddedService();await restoreService.initialize({...context});await restoreService.signIn();
 autoReceipts=['obsolete','current'];restoreService.config.membershipProductType=2;
 const ordinaryRoute=route;let verifiedCurrent=0;
 route=async(url,options)=>{
  if(new URL(url).pathname==='/v1/purchases/verify'){
   if(JSON.parse(options.extraData).purchaseData==='obsolete')throw Error('ORDER_BINDING_MISMATCH');
   verifiedCurrent++;return {status:'ALREADY_FINISHED',finishAllowed:false};
  } return ordinaryRoute(url,options);
 };
 const queriesBefore=subscriptionQueries.length;
 const reconciled=await Promise.allSettled([restoreService.restorePurchases(),restoreService.restorePurchases()]);
 assert.ok(reconciled.every(r=>r.status==='rejected'));assert.equal(verifiedCurrent,2);
 assert.equal(subscriptionQueries.length-queriesBefore,2,'one unfinished/current pagination sequence for concurrent callers');
 route=ordinaryRoute;autoReceipts=[];
 // A response started before an acknowledged purchase cannot overwrite its new entitlement.
 let releaseOld,enteredOld,activeAfterPurchase=false,reads=0;
 const oldEntered=new Promise(r=>enteredOld=r),oldWait=new Promise(r=>releaseOld=r);
 route=async(url,options)=>{
  if(new URL(url).pathname==='/v1/entitlement'){
   reads++;if(reads===1){enteredOld();await oldWait;return {active:false,expiresAt:0,serverTime:Date.now()};}
   return {active:activeAfterPurchase,expiresAt:Date.now()+10000,serverTime:Date.now(),autoRenew:false};
  }
  const result=await ordinaryRoute(url,options);
  if(new URL(url).pathname==='/v1/purchases/verify')activeAfterPurchase=true;
  return result;
 };
 const entitlementService=new mod.exports.ValueAddedService();await entitlementService.initialize({...context});await entitlementService.signIn();
 const delayed=entitlementService.entitlement();await oldEntered;
 // Receipt recovery can invalidate an already-running entitlement read without creating a new purchase.
 await entitlementService.verifyAndFinish('opaque-provider-receipt',3,entitlementService.generation);releaseOld();
 assert.equal((await delayed).active,true);assert.equal(reads,2);route=ordinaryRoute;
 console.log('Account restart: silent verified reauthentication, expired session, explicit logout, receipt isolation, coalesced recovery and stale entitlement PASS');
 // Capability caching is limited to discovery, while explicit refreshes remain real requests.
 const frequencyRoute=route,frequencyCalls=[];
 let gate=null,failRevoke=false,failRegistration=false,registrationReady=true;
 route=async(url,options)=>{
  const endpoint=new URL(url).pathname;frequencyCalls.push(endpoint);
  if(gate?.path===endpoint){const current=gate;gate=null;current.enter();await current.wait;}
  if(endpoint==='/v1/notification-devices/revoke'&&failRevoke)throw Error('OFFLINE');
  if(endpoint==='/v1/notification-devices/register'){
   if(failRegistration)throw Error('OFFLINE');return {ready:registrationReady};
  }
  return frequencyRoute(url,options);
 };
 const count=p=>frequencyCalls.filter(x=>x===p).length;
 const block=path=>{let enter,release;const entered=new Promise(r=>enter=r),wait=new Promise(r=>release=r);gate={path,enter,wait};return {entered,release};};
 const frequent=new mod.exports.ValueAddedService();await frequent.initialize({...context});
 let capabilityCount=count('/v1/capabilities');
 await Promise.all(Array.from({length:20},()=>frequent.refreshCapabilities()));
 assert.equal(count('/v1/capabilities'),capabilityCount);
 frequent.capabilitiesFetchedAt=Date.now()-300001;
 const cacheGate=block('/v1/capabilities');const concurrent=Array.from({length:12},()=>frequent.refreshCapabilities());
 await cacheGate.entered;cacheGate.release();await Promise.all(concurrent);
 assert.equal(count('/v1/capabilities'),++capabilityCount);
 await frequent.refreshCapabilities(true);assert.equal(count('/v1/capabilities'),++capabilityCount);
 // Startup registers once. Unchanged foreground/detail checks never send another cloud request.
 await frequent.signIn();let registrations=count('/v1/notification-devices/register'),revocations=count('/v1/notification-devices/revoke');
 assert.equal(registrations,1);
 const idle=frequencyCalls.length;
 for(let i=0;i<30;i++){frequent.onForeground();await frequent.prepareRenewalReminders(false);}
 assert.equal(frequencyCalls.length,idle,'15-minute-equivalent idle checks remain local');
 notificationsAllowed=false;
 await Promise.all(Array.from({length:12},()=>frequent.prepareRenewalReminders(false)));
 assert.equal(count('/v1/notification-devices/revoke'),++revocations);
 await frequent.prepareRenewalReminders(false);assert.equal(count('/v1/notification-devices/revoke'),revocations);
 notificationsAllowed=true;await frequent.prepareRenewalReminders(false);
 assert.equal(count('/v1/notification-devices/register'),++registrations);
 notificationToken='push-updated-token';const registrationGate=block('/v1/notification-devices/register');
 const repeated=Array.from({length:10},()=>frequent.prepareRenewalReminders(false));await registrationGate.entered;
 notificationToken='push-updated-during-transfer';registrationGate.release();await Promise.all(repeated);
 registrations+=2;assert.equal(count('/v1/notification-devices/register'),registrations);
 assert.ok(frequent.notificationBinding.endsWith(':push-updated-during-transfer'),'token rotation during transfer queues one following registration');
 // Acknowledged but not-ready registrations are also deduplicated; explicit force rechecks the server.
 registrationReady=false;await frequent.prepareRenewalReminders(false,true);registrations++;
 assert.equal(await frequent.prepareRenewalReminders(false),false);assert.equal(count('/v1/notification-devices/register'),registrations);
 registrationReady=true;assert.equal(await frequent.prepareRenewalReminders(false,true),true);registrations++;
 // A failed revoke is not acknowledged locally and can succeed at the next relevant check.
 notificationsAllowed=false;failRevoke=true;await assert.rejects(frequent.prepareRenewalReminders(false),/OFFLINE/);revocations++;
 frequent.onBackground();assert.equal(notificationTimers.has(frequent.notificationRetryTimer),false);
 frequent.onForeground();await frequent.prepareRenewalReminders(false);
 assert.equal(count('/v1/notification-devices/revoke'),revocations,'foreground preserves the pending retry deadline');
 for(const delay of [60000,300000,900000]){
  const timer=notificationTimers.get(frequent.notificationRetryTimer);assert.ok(timer.ms>delay-1000&&timer.ms<=delay);
  notificationTimers.delete(frequent.notificationRetryTimer);frequent.notificationRetryAt=Date.now()-1;timer.f();
  await assert.rejects(frequent.prepareRenewalReminders(false),/OFFLINE/);revocations++;
 }
 assert.equal(frequent.notificationRetryExhausted,true);assert.equal(frequent.notificationRetryTimer,-1);
 for(let i=0;i<10;i++){frequent.onForeground();await frequent.prepareRenewalReminders(false);}
 assert.equal(count('/v1/notification-devices/revoke'),revocations,'exhausted notification retries stay idle');
 failRevoke=false;await frequent.prepareRenewalReminders(false,true);revocations++;
 await frequent.prepareRenewalReminders(false);assert.equal(count('/v1/notification-devices/revoke'),revocations);
 permissionGranted=true;await frequent.prepareRenewalReminders(true,true);registrations++;
 assert.equal(count('/v1/notification-devices/register'),registrations);
 frequent.config.membershipProductType=2;createError=iap.IAPErrorCode.USER_CANCELED;
 const preflightProduct=(await frequent.products()).find(p=>p.type===2);
 assert.equal(await purchaseConfirmed(frequent,preflightProduct),'CANCELLED');
 assert.equal(count('/v1/notification-devices/register'),registrations,'checkout reuses its successful reminder preflight');
 createError=0;frequent.config.membershipProductType=3;
 // Permission restoration cancels an obsolete revoke retry, even if the old binding was never removed.
 notificationsAllowed=false;failRevoke=true;await assert.rejects(frequent.prepareRenewalReminders(false),/OFFLINE/);
 notificationsAllowed=true;assert.equal(await frequent.prepareRenewalReminders(false),true);
 assert.equal(frequent.notificationRetryTimer,-1);
 notificationsAllowed=false;failRevoke=false;await frequent.prepareRenewalReminders(false);
 assert.ok(frequent.notificationBinding.endsWith(':disabled'));
 notificationsAllowed=true;await frequent.prepareRenewalReminders(false);
 // A forced refresh that failed must retry the cloud request even with an older acknowledged binding.
 failRegistration=true;await assert.rejects(frequent.prepareRenewalReminders(false,true),/OFFLINE/);
 const afterFailedForce=count('/v1/notification-devices/register');failRegistration=false;
 const forcedRetry=notificationTimers.get(frequent.notificationRetryTimer);
 notificationTimers.delete(frequent.notificationRetryTimer);frequent.notificationRetryAt=Date.now()-1;forcedRetry.f();
 assert.equal(await frequent.prepareRenewalReminders(false),true);
 assert.equal(count('/v1/notification-devices/register'),afterFailedForce+1);assert.equal(frequent.notificationRetryTimer,-1);
 // Logout waits for an in-flight registration, then revokes it before the next account can register.
 notificationToken='push-logout-race';const logoutGate=block('/v1/notification-devices/register');
 const registering=frequent.prepareRenewalReminders(false);await logoutGate.entered;
 const loggingOut=frequent.signOut();logoutGate.release();await Promise.all([registering,loggingOut]);
 assert.equal(frequent.getUserId(),'');assert.equal(frequent.notificationBinding,'');
 user='other-frequency-owner';await frequent.signIn();assert.match(frequent.notificationBinding,/^other-frequency-owner:/);
 assert.ok(unbindCalls>0&&bindCalls>0);await frequent.signOut();user='alice';notificationToken='push-test-token';route=frequencyRoute;
 console.log('Cloud frequency: 5-minute capability cache/concurrent refresh, idle zero requests, token/permission transitions, registration/revoke dedup, failed revoke and logout race PASS');
 await testLifetimeMembership();
 console.log('Value-added services: hidden guards, auth/ack, account-switch races, profile hash, settings routes/hash/allowlist PASS');
})().finally(()=>{clearTimeout(completionWatchdog);fs.rmSync(directory,{recursive:true,force:true});}).catch(error=>{console.error(error);process.exitCode=1;});
