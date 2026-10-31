// Normalized image/screen coordinates. A homography maps a plane, not a 3D pointing ray.
export const MARKERS = [[.08,.08],[.92,.08],[.92,.92],[.08,.92]];

export function validQuad(points) {
  if (!Array.isArray(points) || points.length !== 4) return false;
  if (points.some(p => !Array.isArray(p) || p.length !== 2 || p.some(v => !Number.isFinite(v)))) return false;
  const turns = points.map((p,i) => {
    const q = points[(i+1)%4], r = points[(i+2)%4];
    return (q[0]-p[0])*(r[1]-q[1])-(q[1]-p[1])*(r[0]-q[0]);
  });
  return turns.every(v => v > 1e-5) || turns.every(v => v < -1e-5);
}

export function homography(source, target = MARKERS) {
  if (!validQuad(source) || !validQuad(target)) throw new Error('Use four distinct, ordered corners: 1, 2, 3, 4.');
  const a = [];
  source.forEach(([x,y],i) => {
    const [u,v] = target[i];
    a.push([x,y,1,0,0,0,-u*x,-u*y,u]);
    a.push([0,0,0,x,y,1,-v*x,-v*y,v]);
  });
  for (let col=0;col<8;col++) {
    let pivot = col;
    for (let r=col+1;r<8;r++) if (Math.abs(a[r][col]) > Math.abs(a[pivot][col])) pivot=r;
    if (Math.abs(a[pivot][col]) < 1e-10) throw new Error('Calibration is degenerate. Use a clearer view.');
    [a[col],a[pivot]]=[a[pivot],a[col]];
    const scale=a[col][col];
    for (let j=col;j<9;j++) a[col][j]/=scale;
    for (let r=0;r<8;r++) if (r!==col) {
      const factor=a[r][col];
      for (let j=col;j<9;j++) a[r][j]-=factor*a[col][j];
    }
  }
  return [...a.map(row=>row[8]),1];
}

export function project(h, point, clip = true) {
  if (!h || h.length!==9 || h.some(v=>!Number.isFinite(v)) ||
      !point || point.length!==2 || point.some(v=>!Number.isFinite(v))) return null;
  const [x,y]=point, z=h[6]*x+h[7]*y+h[8];
  if (Math.abs(z)<1e-9) return null;
  const u=(h[0]*x+h[1]*y+h[2])/z, v=(h[3]*x+h[4]*y+h[5])/z;
  if (!Number.isFinite(u)||!Number.isFinite(v)||(clip&&(u<0||u>1||v<0||v>1))) return null;
  return [u,v];
}

export function validationError(h, imagePoint, expected, width, height) {
  if (!(width>0&&height>0)) throw new Error('Screen dimensions must be positive.');
  const actual=project(h,imagePoint,false);
  if (!actual) return Infinity;
  return Math.hypot((actual[0]-expected[0])*width,(actual[1]-expected[1])*height);
}
