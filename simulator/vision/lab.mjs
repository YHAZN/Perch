import {MARKERS,homography,project,validationError} from './geometry.mjs';
const $=id=>document.getElementById(id), canvas=$('image'), ctx=canvas.getContext('2d');
const screen=$('screen'), sc=screen.getContext('2d');
const demoTransform=[.70,.025,.13,-.04,.72,.15,.12,-.08,1];
let points=[],matrix=null,check=null,checking=false,image=null,source='simulation',live=false;
let generation=0,timer=null,controller=null,ownsCamera=false,probe=null,targetMeta=null;

function error(message){$('error').textContent=message;$('error').hidden=!message;}
function dimensions(){
  const values=['width','height'].map(id=>+$(id).value);
  if(values.some(v=>!Number.isInteger(v)||v<1||v>20000)){error('Enter target dimensions between 1 and 20,000 CSS pixels.');return null;}
  return values;
}
function reset(){points=[];matrix=null;check=null;checking=false;probe=null;draw();status();}
function status(){
  $('source').textContent=source==='simulation'?'Simulation':source==='camera'?'Perch camera':'Local image';
  $('frameState').textContent=live?'Live · freeze to calibrate':source==='simulation'?'Synthetic frame':'Frozen · recalibrate after movement';
  $('quality').textContent=matrix?(source==='simulation'?'Simulation only':'Four-point fit'):'Not calibrated';
  $('measure').textContent=check===null?'Not measured':`${check.toFixed(1)} CSS px${source==='simulation'?' · synthetic':''}`;
  $('step').textContent=live?'Freeze a frame before marking reference points.':checking?'Click the separate centre cross in the camera image.':points.length<4?`Click marker ${points.length+1} centre (${['top left','top right','bottom right','bottom left'][points.length]}).`:'Move over the image to inspect the projection. This does not track your finger.';
  $('freeze').disabled=!live;$('stop').disabled=!ownsCamera;$('export').disabled=!matrix;
  $('validate').disabled=!matrix||live;$('undo').disabled=!points.length||live;$('reset').disabled=live;
}
function dot(context,x,y,label,color='#73b8ff'){
  context.beginPath();context.arc(x,y,7,0,2*Math.PI);context.fillStyle=color;context.fill();
  if(label){context.font='bold 18px sans-serif';context.fillStyle='#f2f7ff';context.fillText(label,x+12,y-10);}
}
function synthetic(){
  ctx.fillStyle='#152234';ctx.fillRect(0,0,canvas.width,canvas.height);
  const corners=[[0,0],[1,0],[1,1],[0,1]].map(p=>project(demoTransform,p,false));
  ctx.beginPath();corners.forEach((p,i)=>ctx[i?'lineTo':'moveTo'](p[0]*640,p[1]*480));ctx.closePath();ctx.fillStyle='#273b50';ctx.fill();
  for(let i=1;i<8;i++){
    for(const ends of [[[i/8,0],[i/8,1]],[[0,i/8],[1,i/8]]]){
      const [a,b]=ends.map(p=>project(demoTransform,p,false));ctx.beginPath();ctx.moveTo(a[0]*640,a[1]*480);ctx.lineTo(b[0]*640,b[1]*480);ctx.strokeStyle='#3b536b';ctx.stroke();
    }
  }
  MARKERS.forEach((p,i)=>{const q=project(demoTransform,p,false);dot(ctx,q[0]*640,q[1]*480,String(i+1),'#f3f7ff');});
  const centre=project(demoTransform,[.5,.5],false);ctx.font='32px sans-serif';ctx.fillStyle='white';ctx.textAlign='center';ctx.fillText('+',centre[0]*640,centre[1]*480+10);ctx.textAlign='left';
}
function draw(){
  ctx.clearRect(0,0,canvas.width,canvas.height);
  if(source==='simulation')synthetic();else if(image)ctx.drawImage(image,0,0,canvas.width,canvas.height);
  if(points.length){ctx.beginPath();points.forEach((p,i)=>ctx[i?'lineTo':'moveTo'](p[0]*canvas.width,p[1]*canvas.height));if(points.length===4)ctx.closePath();ctx.strokeStyle='#72bdff';ctx.lineWidth=2;ctx.stroke();}
  points.forEach((p,i)=>dot(ctx,p[0]*canvas.width,p[1]*canvas.height,String(i+1)));
  sc.fillStyle='#111925';sc.fillRect(0,0,480,270);
  sc.strokeStyle='#34465c';sc.strokeRect(12,12,456,246);
  for(const p of MARKERS)dot(sc,p[0]*480,p[1]*270,'','#465b73');
  if(matrix){
    const aim=project(matrix,[.5,.5]);if(aim)dot(sc,aim[0]*480,aim[1]*270,'','#76bcff');
    const p=probe&&project(matrix,probe);if(p)dot(sc,p[0]*480,p[1]*270,'','#e9cc88');
  }
}
function fit(){matrix=homography(points);check=null;draw();status();}
function stopPolling(){live=false;generation++;clearTimeout(timer);controller?.abort();controller=null;status();}
async function post(path,signal){
  const r=await fetch(path,{method:'POST',signal});const body=await r.json();
  if(!r.ok)throw new Error(body.error||'Device request failed');return body;
}
async function frame(ticket){
  if(!live||ticket!==generation)return;
  controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),15000);
  try{
    const result=await post('/api/preview',controller.signal);
    const next=new Image();next.src=result.image;await next.decode();
    if(!live||ticket!==generation)return;
    image=next;canvas.width=next.naturalWidth;canvas.height=next.naturalHeight;
    // Manual correspondences are invalid after a new frame. No stale tracking.
    reset();error('');
  }catch(e){if(ticket!==generation)return;stopPolling();error(`${e.message}. Camera may still be active; use Stop camera or leave Camera on Perch.`);}
  finally{clearTimeout(timeout);if(live&&ticket===generation)timer=setTimeout(()=>frame(ticket),250);}
}
$('camera').onclick=async()=>{
  stopPolling();error('');$('camera').disabled=true;
  try{
    await post('/api/button/see');ownsCamera=true;source='camera';image=null;reset();live=true;status();frame(generation);
  }catch(e){error(`Could not open Perch camera: ${e.message}`);}
  finally{$('camera').disabled=false;status();}
};
$('freeze').onclick=()=>{stopPolling();error('');};
$('stop').onclick=async()=>{
  stopPolling();
  try{await post('/api/button/home');ownsCamera=false;error('');}catch(e){error(`Could not stop the camera: ${e.message}`);}status();
};
function demo(){
  stopPolling();source='simulation';image=null;canvas.width=640;canvas.height=480;
  points=MARKERS.map(p=>project(demoTransform,p,false));fit();
  check=validationError(matrix,project(demoTransform,[.5,.5],false),[.5,.5],+$('width').value,+$('height').value);
  error('');status();draw();
}
$('demo').onclick=()=>{if(ownsCamera){error('Stop the Perch camera before switching to simulation.');return;}demo();};
$('file').onchange=async()=>{
  if(ownsCamera){error('Stop the Perch camera before opening an image.');return;}
  const file=$('file').files[0];if(!file)return;
  if(file.size>15000000){error('Use an image smaller than 15 MB.');return;}
  stopPolling();const url=URL.createObjectURL(file);
  try{const next=new Image();next.src=url;await next.decode();source='image';image=next;canvas.width=next.naturalWidth;canvas.height=next.naturalHeight;reset();error('');}
  catch{error('Could not read this image.');}finally{URL.revokeObjectURL(url);}
};
$('reset').onclick=()=>{error('');reset();};
$('undo').onclick=()=>{points.pop();matrix=null;check=null;checking=false;status();draw();};
$('validate').onclick=()=>{checking=true;status();};
canvas.onpointerdown=e=>{
  if(live)return;
  const r=canvas.getBoundingClientRect(),p=[(e.clientX-r.left)/r.width,(e.clientY-r.top)/r.height];
  if(checking){const size=dimensions();if(!size)return;check=validationError(matrix,p,[.5,.5],...size);checking=false;status();return;}
  if(points.length===4)return;
  points.push(p);try{if(points.length===4)fit();error('');}catch(e){matrix=null;error(e.message);}status();draw();
};
canvas.onpointermove=e=>{const r=canvas.getBoundingClientRect();probe=[(e.clientX-r.left)/r.width,(e.clientY-r.top)/r.height];draw();};
canvas.onpointerleave=()=>{probe=null;draw();};
for(const id of ['width','height'])$(id).onchange=()=>{check=null;status();};
$('export').onclick=()=>{
  if(!matrix)return;
  const size=dimensions();if(!size)return;
  const result={version:1,source,sourceSize:[canvas.width,canvas.height],markerImagePoints:points,markerScreenPoints:MARKERS,
    matrix,targetCssPixels:size,centreCheckCssPixels:Number.isFinite(check)?check:null,
    targetFullscreen:targetMeta?.fullscreen??null,created:new Date().toISOString(),
    scope:'Frozen planar calibration only; no finger depth, distortion correction, automatic tracking or host input.'};
  const url=URL.createObjectURL(new Blob([JSON.stringify(result,null,2)],{type:'application/json'}));
  const link=document.createElement('a');link.href=url;link.download='screen-calibration.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
};
const channel=new BroadcastChannel('perch-calibration-target');
channel.onmessage=({data})=>{
  if(!data||!Number.isFinite(data.width)||!Number.isFinite(data.height)||data.width<1||data.height<1)return;
  const changed=!targetMeta||data.width!==targetMeta.width||data.height!==targetMeta.height||data.fullscreen!==targetMeta.fullscreen;
  targetMeta=data;$('width').value=data.width;$('height').value=data.height;
  if(changed&&source!=='simulation'){reset();error('Target size changed. Capture a new frame and recalibrate.');}
  status();
};
document.addEventListener('visibilitychange',()=>{if(document.hidden&&live)stopPolling();});
demo();
