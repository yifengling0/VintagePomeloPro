import test from 'node:test';
import assert from 'node:assert/strict';
import {once} from 'node:events';
import {createApiServer} from '../src/http-server.mjs';
test('HTTP settings routes pass authenticated identity and device class, never a client owner',async()=>{
  const calls=[];const envelope={revision:1};
  const server=createApiServer({authenticate:async()=> 'a'.repeat(64),
    getPreferences:async(...args)=>{calls.push(args);return {current:envelope};},
    putPreferences:async(...args)=>{calls.push(args);return {status:'SAVED',current:envelope};}});
  server.listen(0,'127.0.0.1');await once(server,'listening');const base='http://127.0.0.1:'+server.address().port;
  const headers={authorization:'Bearer '+'a'.repeat(43),'content-type':'application/json'};
  try {
    const read=await fetch(base+'/v1/settings/tablet',{headers});assert.equal(read.status,200);assert.equal((await read.json()).current.revision,1);
    const write=await fetch(base+'/v1/settings/desktop',{method:'PUT',headers,body:JSON.stringify({baseRevision:0,payload:{themeMode:'dark'},userId:'attacker'})});
    assert.equal(write.status,200);assert.equal((await write.json()).status,'SAVED');
    assert.equal(calls[0][0],'a'.repeat(64));assert.equal(calls[0][1],'tablet');assert.equal(calls[1][0],'a'.repeat(64));assert.equal(calls[1][1],'desktop');
    assert.equal((await fetch(base+'/v1/settings/phone',{headers})).status,404);
    assert.equal((await fetch(base+'/v1/settings/tablet',{method:'DELETE',headers})).status,404);
  } finally {server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
});
