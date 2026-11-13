#!/usr/bin/env node
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const ts=require(process.argv[2]);
let visible=true,price=8,pending=[],createCalls=[],finishCalls=[],failure=0,failFinish=false,finishErrorCode=0,onCreateFailure=null,queryHook=null;
const receipt=(productId='buyme_coffee',quantity=1,id='order-1',extra={})=>JSON.stringify({jwsPurchaseOrder:
 'header.'+Buffer.from(JSON.stringify({productId,quantity,purchaseOrderId:id,purchaseToken:'token-'+id,applicationId:'configured-app',productType:'0',...extra})).toString('base64url')+'.signature'});
const iap={ProductType:{CONSUMABLE:0},PurchaseQueryType:{UNFINISHED:1},IAPErrorCode:{USER_CANCELED:1,PRODUCT_OWNED:2,SYSTEM_ERROR:1001860001},
 queryProducts:async()=>[{id:'buyme_coffee',type:0,microPrice:price*1000000,currency:'CNY'}],
 queryPurchases:async(_,args)=>queryHook?queryHook(args):({purchaseDataList:pending}),
 createPurchase:async(_,args)=>{createCalls.push(args);if(failure){if(onCreateFailure)onCreateFailure();throw {code:failure};}return {purchaseData:receipt(args.productId,args.quantity)};},
 finishPurchase:async(_,args)=>{finishCalls.push(args);if(finishErrorCode)throw {code:finishErrorCode};if(failFinish)throw Error('offline');}};
const util={TextDecoder:{create:()=>({decodeToString:b=>Buffer.from(b).toString('utf8')})},Base64Helper:class{decodeSync(s){return Buffer.from(s,'base64');}}};
const imports={'@kit.AbilityKit':{},'@kit.BasicServicesKit':{deviceInfo:{sdkApiVersion:20}},'@kit.IAPKit':{iap},'@ohos.util':{default:util},
 '../../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:{supportsValueAddedFeatures:()=>visible}}};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/valueadded/CoffeePurchaseService.ets'),'utf8');
const js=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
const mod={exports:{}};vm.runInNewContext(js,{module:mod,exports:mod.exports,require:n=>imports[n],Uint8Array,console});
const service=mod.exports.CoffeePurchaseService.getInstance();
const context={resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify({productId:'buyme_coffee',appId:'configured-app',unitPriceCny:8}))}};
(async()=>{
 visible=false;await assert.rejects(service.initialize(context),/FEATURE_HIDDEN/);visible=true;await service.initialize(context);
 for(const quantity of [1,4,10]){assert.equal(await service.purchase(quantity,{amount:8,currency:'CNY'}),'FINISHED');assert.equal(createCalls.at(-1).quantity,quantity);}
 assert.deepEqual(createCalls.map(x=>x.quantity*8),[8,32,80]);
 imports['@kit.BasicServicesKit'].deviceInfo.sdkApiVersion=14;
 await assert.rejects(service.purchase(4,{amount:8,currency:'CNY'}),/QUANTITY_UNSUPPORTED/);
 assert.equal(createCalls.length,3,'API without quantities must not create a differently priced order');
 imports['@kit.BasicServicesKit'].deviceInfo.sdkApiVersion=20;
 assert.equal(finishCalls.length,3);await assert.rejects(service.purchase(2,{amount:8,currency:'CNY'}),/QUANTITY/);
 price=9;await assert.rejects(service.purchase(1,{amount:8,currency:'CNY'}),/PRICE_CHANGED/);assert.equal(createCalls.length,3);price=8;
 pending=[receipt('membership',1,'other'),receipt('buyme_coffee',4,'paid')];
 assert.equal(await service.purchase(1,{amount:8,currency:'CNY'}),'RECOVERED');assert.equal(createCalls.length,3);
 assert.equal(finishCalls.at(-1).purchaseOrderId,'paid');assert.ok(!finishCalls.some(x=>x.purchaseOrderId==='other'));
 failFinish=true;assert.equal(await service.purchase(1,{amount:8,currency:'CNY'}),'ACK_PENDING');assert.equal(createCalls.length,3);
 failFinish=false;pending=[];failure=1;assert.equal(await service.purchase(1,{amount:8,currency:'CNY'}),'CANCELLED');
 failure=0;failFinish=true;assert.equal(await service.purchase(1,{amount:8,currency:'CNY'}),'ACK_PENDING');failFinish=false;
 pending=[JSON.stringify({jwsPurchaseOrder:'bad'})];await assert.rejects(service.recoverPurchases(),/ORDER_INVALID/);pending=[];
 assert.equal(await service.recoverPurchases(),'NO_PENDING');
 // Official PurchaseOrderPayload names, rather than a mock-only appId field.
 pending=[receipt('buyme_coffee',1,'wrong-app',{applicationId:'other-app'})];
 const beforeForeign=finishCalls.length;
 assert.equal(await service.recoverPurchases(),'ACK_PENDING');assert.equal(finishCalls.length,beforeForeign);
 pending=[receipt('buyme_coffee',1,'legacy-app',{applicationId:undefined,appId:'configured-app'})];
 assert.equal(await service.recoverPurchases(),'ACK_PENDING');assert.equal(finishCalls.length,beforeForeign);
 pending=[receipt('buyme_coffee',1,'wrong-type',{productType:'2'})];
 assert.equal(await service.recoverPurchases(),'ACK_PENDING');assert.equal(finishCalls.length,beforeForeign);
 pending=[];
 // Finalized-race codes are successful only after official UNFINISHED confirms absence.
 for(const code of [1001860030,1001860050]){
  finishErrorCode=code;
  assert.equal(await service.purchase(1,{amount:8,currency:'CNY'}),'FINISHED');
  pending=[receipt()];assert.equal(await service.recoverPurchases(),'ACK_PENDING');pending=[];
 }
 finishErrorCode=0;
 // A payment transport error may still leave a paid order. Recover it without a second create.
 failure=1001860001;onCreateFailure=()=>pending=[receipt('buyme_coffee',4,'paid-during-system-error')];
 const beforeSystem=createCalls.length;
 assert.equal(await service.purchase(4,{amount:8,currency:'CNY'}),'RECOVERED');assert.equal(createCalls.length,beforeSystem+1);
 assert.equal(finishCalls.at(-1).purchaseOrderId,'paid-during-system-error');
 pending=[];onCreateFailure=null;
 await assert.rejects(service.purchase(1,{amount:8,currency:'CNY'}),/COFFEE_IAP_1001860001/);failure=0;
 // All pages are read; duplicate orders are confirmed once and pagination cycles fail closed.
 queryHook=({continuationToken})=>continuationToken?{purchaseDataList:[receipt('buyme_coffee',1,'page-2'),receipt('buyme_coffee',1,'page-1')]}:
  {purchaseDataList:[receipt('buyme_coffee',1,'page-1')],continuationToken:'next'};
 const beforePages=finishCalls.length;assert.equal(await service.recoverPurchases(),'RECOVERED');assert.equal(finishCalls.length,beforePages+2);
 queryHook=()=>({purchaseDataList:[],continuationToken:'loop'});await assert.rejects(service.recoverPurchases(),/ORDER_INVALID/);queryHook=null;
 console.log('Coffee SDK flow: 8/32/80, direct IAP, receipt ownership, price changes, old orders without repeat charge, cancellation and retry PASS (mock SDK; no real payment)');
})().catch(error=>{console.error(error);process.exitCode=1;});
