// Explicit operator-run probe only. Writes/deletes a small random probe object; never player saves.
import { randomUUID, randomBytes, createHash } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { connectObs, OBS_VERSION } from '../src/obs-objects.mjs';
import { hash } from '../src/save-manifest.mjs';
import { requireThat } from '../src/errors.mjs';
export async function probeObs(objects) {
  await objects.checkBucketSecurity();
  const key='saves/'+randomBytes(32).toString('hex')+'/'+randomUUID()+'/0',bytes=Buffer.from('VintagePomelo object-integrity probe v1');let cleanup=false;
  try {
    cleanup=true;await objects.put(key,bytes,hash(bytes));
    const signed=objects.sign('GET',key),anonymous=new URL(signed.url);anonymous.search='';
    const response=await objects.transport(anonymous.href,{redirect:'error',signal:AbortSignal.timeout(10000)});
    requireThat([403,404].includes(response.status),'OBS_BUCKET_NOT_PRIVATE');await response.body?.cancel();
    const bad=await objects.request('PUT',key,{'Content-Type':'application/octet-stream','Content-MD5':createHash('md5').update(bytes).digest('base64'),'x-obs-content-sha256':'0'.repeat(64)},bytes);
    requireThat(bad.status===400,'OBS_BAD_CHECKSUM_ACCEPTED');await bad.body?.cancel();
    requireThat(hash(await objects.read(key,bytes.length))===hash(bytes),'OBS_BAD_WRITE_CHANGED_OBJECT');
    await objects.delete(key);cleanup=false;
    const missing=await objects.request('GET',key);
    requireThat(missing.status===404,'OBS_DELETE_NOT_VERIFIED');await missing.body?.cancel();
    return {fingerprint:objects.fingerprint,sdkVersion:OBS_VERSION,privateBucket:true,bpaChecked:true,savesNamespacePrivate:true,versioningDisabled:true,
      roundTripSha256:true,badChecksumRejected:true,deleteVerified:true,verifiedAt:new Date().toISOString()};
  } finally {if(cleanup)await objects.delete(key);}
}
if(process.argv[1]&&new URL(import.meta.url).pathname===process.argv[1]) {
  try {
    requireThat(process.env.VP_PROBE_ALLOW_WRITES==='I_APPROVE_TEST_RECORDS'&&process.env.VP_OBS_VERIFICATION_FILE,'PROBE_NOT_AUTHORIZED');
    const result=await probeObs(await connectObs(process.env));
    await writeFile(process.env.VP_OBS_VERIFICATION_FILE,JSON.stringify(result,null,2)+'\n',{mode:0o600,flag:'wx'});
    console.log('OBS private-bucket/checksum/round-trip/delete probe PASS. Report written; no player data used.');
  } catch(error){console.error(error.code??'OBS_PROBE_FAILED');process.exitCode=1;}
}
