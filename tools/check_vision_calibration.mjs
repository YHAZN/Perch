import test from 'node:test';
import assert from 'node:assert/strict';
import {homography,project,validQuad,validationError,MARKERS} from '../simulator/vision/geometry.mjs';
const near=(a,b)=>a.forEach((v,i)=>assert.ok(Math.abs(v-b[i])<1e-8,`${a} != ${b}`));
test('Inset marker centres map to their actual positions, not the screen edges',()=>{
  const h=homography(MARKERS);
  near(project(h,[.5,.5]),[.5,.5]);
  near(project(h,[.08,.08]),[.08,.08]);
});
test('Perspective mapping generalizes to held-out points on the same plane',()=>{
  const screenToImage=[.7,.08,.1,-.03,.65,.17,.15,-.12,1];
  const imageMarkers=MARKERS.map(p=>project(screenToImage,p,false));
  const h=homography(imageMarkers);
  for(const p of [[.5,.5],[.25,.7],[.8,.3]]) near(project(h,project(screenToImage,p,false)),p);
  assert.ok(validationError(h,project(screenToImage,[.5,.5],false),[.5,.5],1920,1080)<1e-8);
});
test('Crossed, repeated, collinear and non-finite points are rejected',()=>{
  for(const p of [[[0,0],[1,1],[1,0],[0,1]],[[0,0],[1,0],[1,0],[0,1]],
                   [[0,0],[.2,0],[.4,0],[.6,0]],[[NaN,0],[1,0],[1,1],[0,1]]]) {
    assert.equal(validQuad(p),false); assert.throws(()=>homography(p));
  }
});
test('Horizon and outside-screen projections freeze rather than clamping to an edge',()=>{
  assert.equal(project([1,0,0,0,1,0,0,0,1],[-.1,.5]),null);
  assert.equal(project([1,0,0,0,1,0,1,0,-.5],[.5,.5]),null);
});
test('Held-out validation reports error independently of the four fitted points',()=>{
  const h=homography(MARKERS);
  assert.ok(validationError(h,[.6,.5],[.5,.5],1000,500)>99.9);
});
