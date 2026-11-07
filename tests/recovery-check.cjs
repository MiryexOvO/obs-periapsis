// Isolated real OBS -> PERI Relay -> recovery request -> decoded fresh GOP.
// 隔离验证真实 OBS 发布、Relay 反馈和恢复 GOP 解码，不访问用户直播配置。
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {spawn, execFileSync} = require('node:child_process');
const core = path.resolve(__dirname, '../../periapsis');
const dir = fs.mkdtempSync(path.join(core, 'dist/obs-recovery-'));
const obsBin = process.env.PERI_OBS_BIN || 'D:/OBS/obs-studio/bin/64bit';
const ffmpeg = process.env.PERI_FFMPEG || path.join(process.env.USERPROFILE, 'scoop/shims/ffmpeg.exe');
const env = {...process.env, PATH:obsBin+';'+process.env.PATH,
  PERI_PUBLISH_TOKEN:crypto.randomBytes(32).toString('hex'),
  PERI_SUBSCRIBE_TOKEN:crypto.randomBytes(32).toString('hex'),
  PERI_OBS_TEST_CONFIG:path.join(dir,'obs-config')};
fs.mkdirSync(env.PERI_OBS_TEST_CONFIG);
const jobs = [];
function run(name, bin, args, cwd = core) {
  const p = spawn(bin,args,{env,cwd,windowsHide:true});
  const job = {p,name,log:''};
  for(const s of [p.stdout,p.stderr]) s.on('data',b=>job.log+=b);
  job.done = new Promise((resolve,reject)=>{p.once('error',reject);p.once('close',resolve)});
  job.done.catch(()=>{}); jobs.push(job); return job;
}
const sleep = ms=>new Promise(r=>setTimeout(r,ms));
async function wait(job,re) {
  for(let i=0;i<200;i++) {
    const m = job.log.match(re); if(m) return m;
    if(job.p.exitCode!==null) break;
    await sleep(50);
  }
  throw Error('missing '+re+' in '+job.name);
}
async function finish(job,ms) {
  let timer;
  try { return await Promise.race([job.done,new Promise((_,reject)=>{
    timer=setTimeout(()=>reject(Error(job.name+' timed out')),ms);
  })]); } finally {clearTimeout(timer)}
}
(async()=>{try {
  const bin=path.join(core,'target/release/peri.exe');
  execFileSync(bin,['init',path.join(dir,'tls')],{env,stdio:'pipe'});
  const cert=path.join(dir,'tls/cert.der'), resource='live/obs-recovery';
  const relay=run('relay',bin,['relay','127.0.0.1:0',cert,path.join(dir,'tls/key.key'),resource]);
  const address=(await wait(relay,/PERI_RELAY_READY (\S+)/))[1];
  env.PERI_OBS_TEST_TICKET='peri://v1/'+Buffer.from(JSON.stringify({address,name:'localhost',resource,
    token:env.PERI_PUBLISH_TOKEN,cert:fs.readFileSync(cert).toString('base64')})).toString('base64url');
  const source=run('source',path.join(core,'target/obs-sdk/obs-recovery-source.exe'),[],obsBin);
  await wait(relay,/PERI_RESOURCE_READY/);
  const ivf=path.join(dir,'recovered.ivf');
  const probe=run('probe',path.join(core,'target/release/examples/obs_recovery.exe'),[address,cert,resource,ivf]);
  if(await finish(probe,15000)!==0) throw Error('recovery probe failed');
  const result=JSON.parse((await wait(probe,/OBS_RECOVERY_RESULT (.+)/))[1]);
  const identity=`origin=${result.origin} id=${result.id}`;
  if(!source.log.includes('Recovery submitted / 已提交编码恢复: '+identity) ||
     !source.log.includes('Recovery key observed / 已观察编码关键帧: '+identity))
    throw Error('missing encoder response evidence');
  const decoded=execFileSync(ffmpeg,['-hide_banner','-loglevel','error','-i',ivf,'-map','0:v:0','-fps_mode','passthrough','-f','framemd5','pipe:1'],{env,timeout:15000}).toString();
  const hashes=decoded.split(/\r?\n/).filter(x=>x && !x.startsWith('#'));
  if(hashes.length!==30) throw Error('expected 30 independently decoded recovery frames');
  fs.writeFileSync(path.join(dir,'decoded.framemd5'),decoded);
  if(await finish(source,15000)!==0) throw Error('OBS did not shut down cleanly');
  result.decoded_frames=hashes.length; result.obs_exit=source.p.exitCode;
  fs.writeFileSync(path.join(dir,'report.json'),JSON.stringify(result,null,2));
  console.log(JSON.stringify({directory:dir,...result}));
} finally {
  for(const job of jobs.reverse()) {
    if(job.p.exitCode===null && !job.p.killed) job.p.kill();
    let text=job.log;
    for(const secret of [env.PERI_PUBLISH_TOKEN,env.PERI_SUBSCRIBE_TOKEN,env.PERI_OBS_TEST_TICKET])
      if(secret) text=text.split(secret).join('[redacted]');
    fs.writeFileSync(path.join(dir,job.name+'.log'),text);
  }
}})().catch(e=>{console.error(e.message,dir);process.exitCode=1});
