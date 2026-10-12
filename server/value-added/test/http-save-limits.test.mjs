import test from 'node:test';
import assert from 'node:assert/strict';
import { request } from 'node:http';
import { once } from 'node:events';
import { createApiServer } from '../src/http-server.mjs';
import { CHUNK_BYTES } from '../src/save-manifest.mjs';
test('HTTP rejects oversized binary declarations before lease/body and enforces exact bytes',async()=>{
 let acquired=0,uploads=0;
 const server=createApiServer({authenticate:async()=> 'a'.repeat(64),saves:{
  acquireChunk:async()=>{acquired++;return {expected:{size:4}};},
  upload:async(_u,_id,_part,bytes)=>{uploads++;assert.equal(bytes.toString(),'save');return {status:'STORED'};}
 }});
 server.listen(0,'127.0.0.1');await once(server,'listening');
 const call=(length,body,type='application/octet-stream')=>new Promise((resolve,reject)=>{
  const req=request({host:'127.0.0.1',port:server.address().port,method:'PUT',
   path:'/v1/saves/uploads/11111111-1111-1111-1111-111111111111/chunks/0',
   headers:{authorization:'Bearer '+'a'.repeat(43),'content-type':type,'content-length':length}},res=>{
    const data=[];res.on('data',chunk=>data.push(chunk));res.on('end',()=>resolve({status:res.statusCode,body:JSON.parse(Buffer.concat(data))}));
   });req.on('error',reject);req.end(body);
 });
 try {
  assert.equal((await call(CHUNK_BYTES+1,'x')).status,413);assert.equal(acquired,0);
  assert.equal((await call(4,'save','text/plain')).status,413);assert.equal(acquired,0);
  assert.equal((await call(3,'bad')).status,413);assert.equal(uploads,0);
  assert.equal((await call(4,'save')).status,200);assert.equal(uploads,1);
 } finally {server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
});
