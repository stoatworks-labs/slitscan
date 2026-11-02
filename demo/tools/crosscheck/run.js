// The page's half of the crosscheck: fetch the cases and the harness's card
// frames from crosscheck.py's server, render each case through
// window.slitscanDemo.renderCase, and post the picture back. crosscheck.py
// compares; nothing is judged here.
const post = (path, body) => fetch(path, { method: 'POST', body });

async function main() {
  for (let i = 0; i < 200 && !window.slitscanDemo; i += 1) await new Promise((r) => setTimeout(r, 50));
  if (!window.slitscanDemo) throw new Error('window.slitscanDemo never appeared: the page did not start');
  const job = await (await fetch('/xc/job.json')).json();
  const frames = new Uint8Array(await (await fetch('/xc/cards.rgba')).arrayBuffer());
  for (const c of job.cases) {
    const out = window.slitscanDemo.renderCase({
      settings: c.settings, frames, width: job.width, height: job.height, fps: job.fps, count: c.count,
    });
    await post(`/xc/out/${c.name}`, out);
  }
  await post('/xc/done', 'ok');
}

main().catch((error) => post('/xc/error', String(error?.stack ?? error)));
