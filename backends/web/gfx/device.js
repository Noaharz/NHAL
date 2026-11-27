// gfx.device – Web: der größere JS-Teil (Gerät, Layouts, Pipelines, Passes,
// Push-Konstanten). Als --js-library gelinkt (js_libraries im Katalog), weil
// der Code für EM_JS zu groß ist. Die kleinen Aufrufe stehen in device.c.
//
// Beschreibungen kommen als flache uint32-Folgen aus C (HEAPU32); Gleitkomma-
// werte liegen an denselben Stellen und werden über HEAPF32 gelesen.
addToLibrary({
  nhal_web_gpu_init__deps: ["$UTF8ToString"],
  nhal_web_gpu_init: function () {
      const g = globalThis.nhalGpu = {
          state: 0, reason: 0, device: null, name: "", errors: [], done: [], objs: [null], free: [],
          submitted: 0, completed: 0, ts: false, fi: false, pool: [],
          formats: ["", "r8unorm", "rgba8unorm", "rgba8unorm-srgb", "bgra8unorm", "bgra8unorm-srgb",
                    "r32float", "rg32float", "rgba16float", "rgba32float", "depth32float"],
          vformats: ["", "float32", "float32x2", "float32x3", "float32x4", "unorm8x4", "uint32"],
          compares: ["less", "less-equal", "greater", "greater-equal", "equal", "not-equal", "always", "never"]
      };
      // Push-Konstanten: 64-KiB-Blöcke, je Draw 256 Bytes, gebunden mit 128 Bytes Größe.
      const CHUNK = 65536, STEP = 256, CONST_SIZE = 128;
      g.add = (o) => { const id = g.free.length ? g.free.pop() : g.objs.length; g.objs[id] = o; return id; };
      g.del = (id) => { if (id > 0 && id < g.objs.length) { g.objs[id] = null; g.free.push(id); } };

      // Bind-Group-Layout aus (slot, kind, stages, format)-Vierern.
      g.bgl = (u32, at, count) => {
          const entries = [];
          const bindings = [];
          for (let i = 0; i < count; ++i) {
              const slot = u32[at++], kind = u32[at++], stages = u32[at++], fmt = u32[at++];
              const e = { binding: slot, visibility: stages & 7 };
              if (kind === 1) e.buffer = { type: "uniform" };
              else if (kind === 2) e.buffer = { type: "storage" };
              else if (kind === 3) e.buffer = { type: "read-only-storage" };
              else if (kind === 4) e.texture = { sampleType: "float" };
              else if (kind === 5) e.storageTexture = { access: "write-only", format: g.formats[fmt] };
              else e.sampler = { type: "filtering" };
              entries.push(e);
              bindings.push({ slot: slot, kind: kind });
          }
          return { bgl: g.device.createBindGroupLayout({ entries: entries }), bindings: bindings, at: at };
      };
      g.resource = (kind, id, offset, size) => {
          const o = g.objs[id];
          if (kind <= 3) {
              const res = { buffer: o, offset: offset };
              if (size > 0) res.size = size;
              return res;
          }
          return kind <= 5 ? o.createView() : o;
      };
      g.empty = () => {
          if (!g.emptyBgl) {
              g.emptyBgl = g.device.createBindGroupLayout({ entries: [] });
              g.emptyBg = g.device.createBindGroup({ layout: g.emptyBgl, entries: [] });
          }
      };
      g.constLayout = (compute) => {
          const key = compute ? "constC" : "constR";
          if (!g[key]) {
              g[key] = g.device.createBindGroupLayout({ entries: [{
                  binding: 0, visibility: compute ? GPUShaderStage.COMPUTE : GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
                  buffer: { type: "uniform", hasDynamicOffset: true, minBindingSize: CONST_SIZE } }] });
          }
          return g[key];
      };
      // Pipeline-Layout: [Gruppe 0..2 (Slots, eigene oder leer), Gruppe 3 = Konstanten].
      // Liest: g0 g1 g2 (Layout-Nummern), constants, Slot-Anzahl, Slot-Vierer.
      g.pipelineLayout = (u32, at, compute) => {
          const ids = [u32[at++], u32[at++], u32[at++]];
          const consts = u32[at++] !== 0;
          const slotCount = u32[at++];
          const slots = slotCount ? g.bgl(u32, at, slotCount) : null;
          if (slots) at = slots.at;
          const kinds = [], bgls = [];
          let n = consts ? 4 : 0;
          for (let i = 2; i >= 0 && !n; --i) if (ids[i] || (i === 0 && slots)) n = i + 1;
          g.empty();
          for (let i = 0; i < n; ++i) {
              if (i === 3) { kinds.push(3); bgls.push(g.constLayout(compute)); }
              else if (i === 0 && slots) { kinds.push(1); bgls.push(slots.bgl); }
              else if (ids[i]) { kinds.push(2); bgls.push(g.objs[ids[i]].bgl); }
              else { kinds.push(0); bgls.push(g.emptyBgl); }
          }
          return { layout: g.device.createPipelineLayout({ bindGroupLayouts: bgls }), kinds: kinds,
                   slotBgl: slots ? slots.bgl : null, bindings: slots ? slots.bindings : [], compute: compute, at: at };
      };
      g.blendState = (b) => {
          const f = (s, d) => ({ srcFactor: s, dstFactor: d, operation: "add" });
          if (b === 1) return { color: f("src-alpha", "one-minus-src-alpha"), alpha: f("one", "one-minus-src-alpha") };
          if (b === 2) return { color: f("one", "one-minus-src-alpha"), alpha: f("one", "one-minus-src-alpha") };
          if (b === 3) return { color: f("src-alpha", "one"), alpha: f("one", "one") };
          if (b === 4) return { color: f("dst", "zero"), alpha: f("dst-alpha", "zero") };
          return undefined;
      };
      g.renderPipeline = (desc) => {
          const u = HEAPU32;
          let at = desc >> 2;
          const module = g.objs[u[at++]];
          const vs = UTF8ToString(u[at++]);
          const fsPtr = u[at++];
          const colorCount = u[at++];
          const targets = [];
          for (let i = 0; i < colorCount; ++i) {
              const fmt = u[at++], blend = u[at++], mask = u[at++];
              const t = { format: g.formats[fmt], writeMask: mask || 0xF };
              const bs = g.blendState(blend);
              if (bs) t.blend = bs;
              targets.push(t);
          }
          const depth = u[at++], depthTest = u[at++], depthWrite = u[at++], compare = u[at++];
          const topology = u[at++], cull = u[at++], front = u[at++], samples = u[at++], vbCount = u[at++];
          const buffers = [];
          for (let i = 0; i < vbCount; ++i) {
              const stride = u[at++], inst = u[at++], n = u[at++];
              const attributes = [];
              for (let a = 0; a < n; ++a) {
                  const fmt = u[at++], offset = u[at++], loc = u[at++];
                  attributes.push({ format: g.vformats[fmt], offset: offset, shaderLocation: loc });
              }
              buffers.push({ arrayStride: stride, stepMode: inst ? "instance" : "vertex", attributes: attributes });
          }
          const l = g.pipelineLayout(u, at, false);
          const d = {
              layout: l.layout,
              vertex: { module: module, entryPoint: vs, buffers: buffers },
              primitive: {
                  topology: ["triangle-list", "triangle-strip", "line-list", "point-list"][topology],
                  frontFace: front ? "cw" : "ccw", cullMode: ["none", "back", "front"][cull]
              },
              multisample: { count: samples }
          };
          if (fsPtr) d.fragment = { module: module, entryPoint: UTF8ToString(fsPtr), targets: targets };
          if (depth) {
              d.depthStencil = { format: "depth32float", depthWriteEnabled: depthWrite !== 0,
                                 depthCompare: depthTest ? g.compares[compare] : "always" };
          }
          l.pipe = g.device.createRenderPipeline(d);
          return g.add(l);
      };
      g.computePipeline = (desc) => {
          const u = HEAPU32;
          let at = desc >> 2;
          const module = g.objs[u[at++]];
          const entry = UTF8ToString(u[at++]);
          const l = g.pipelineLayout(u, at, true);
          l.pipe = g.device.createComputePipeline({ layout: l.layout, compute: { module: module, entryPoint: entry } });
          return g.add(l);
      };

      // Liest: cmd, colorCount, je Farbe (tex, resolve, load, store, r, g, b, a), depth, depthLoad, depthStore,
      // clearDepth, occlusion, tsSet, tsBegin, tsEnd.
      g.renderPassBegin = (desc) => {
          const u = HEAPU32, f = HEAPF32;
          let at = desc >> 2;
          const cmd = g.objs[u[at++]];
          const n = u[at++];
          const d = { colorAttachments: [] };
          for (let i = 0; i < n; ++i) {
              const tex = u[at], res = u[at + 1], load = u[at + 2], store = u[at + 3];
              const c = { view: g.objs[tex].createView(), loadOp: load ? "load" : "clear",
                          clearValue: { r: f[at + 4], g: f[at + 5], b: f[at + 6], a: f[at + 7] },
                          storeOp: store ? "discard" : "store" };
              if (res) c.resolveTarget = g.objs[res].createView();
              d.colorAttachments.push(c);
              at += 8;
          }
          const depth = u[at++], dload = u[at++], dstore = u[at++], clear = f[at++];
          if (depth) {
              d.depthStencilAttachment = { view: g.objs[depth].createView(), depthLoadOp: dload ? "load" : "clear",
                                           depthClearValue: clear, depthStoreOp: dstore ? "discard" : "store" };
          }
          const occ = u[at++], ts = u[at++], tb = u[at++], te = u[at++];
          if (occ) d.occlusionQuerySet = g.objs[occ];
          if (ts) d.timestampWrites = { querySet: g.objs[ts], beginningOfPassWriteIndex: tb, endOfPassWriteIndex: te };
          return g.add(g.newPass(cmd, cmd.enc.beginRenderPass(d)));
      };
      g.newPass = (cmd, enc) => ({ enc: enc, cmd: cmd, pipe: null, bound: [], dirty: true, groups: [null, null, null],
                                   consts: null, applied: [] });

      // Vor jedem Draw/Dispatch: alle Gruppen der Pipeline setzen, die sich geändert haben.
      g.flush = (p) => {
          const pl = p.pipe;
          if (!pl) return;
          for (let i = 0; i < pl.kinds.length; ++i) {
              const k = pl.kinds[i];
              if (k === 1) {
                  if (!p.dirty && p.applied[i] === "slots") continue;
                  const entries = pl.bindings.map((b) => {
                      const r = p.bound[b.slot];
                      return { binding: b.slot, resource: g.resource(b.kind, r.id, r.offset, r.size) };
                  });
                  p.enc.setBindGroup(i, g.device.createBindGroup({ layout: pl.slotBgl, entries: entries }));
                  p.applied[i] = "slots";
              } else if (k === 2) {
                  const bg = p.groups[i];
                  if (p.applied[i] !== bg) { p.enc.setBindGroup(i, bg); p.applied[i] = bg; }
              } else if (k === 0) {
                  if (p.applied[i] !== g.emptyBg) { p.enc.setBindGroup(i, g.emptyBg); p.applied[i] = g.emptyBg; }
              } else {
                  const c = p.consts;
                  const bg = pl.compute ? c.chunk.bgC : c.chunk.bgR;
                  const key = c.chunk.id * CHUNK + c.off + (pl.compute ? 0.5 : 0);
                  if (p.applied[i] !== key) { p.enc.setBindGroup(i, bg, [c.off]); p.applied[i] = key; }
              }
          }
          p.dirty = false;
      };

      g.takeChunk = () => {
          let c = g.pool.pop();
          if (!c) {
              const buf = g.device.createBuffer({ size: CHUNK, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
              const entry = (l) => g.device.createBindGroup({ layout: l, entries: [{ binding: 0,
                  resource: { buffer: buf, offset: 0, size: CONST_SIZE } }] });
              c = { id: (g.chunkIds = (g.chunkIds || 0) + 1), buf: buf, data: new Uint8Array(CHUNK), off: 0,
                    bgR: entry(g.constLayout(false)), bgC: entry(g.constLayout(true)) };
          }
          c.off = 0;
          return c;
      };
      g.constants = (p, ptr, size) => {
          const cmd = p.cmd;
          let c = cmd.chunks.length ? cmd.chunks[cmd.chunks.length - 1] : null;
          if (!c || c.off + STEP > CHUNK) { c = g.takeChunk(); cmd.chunks.push(c); }
          c.data.fill(0, c.off, c.off + CONST_SIZE);
          c.data.set(HEAPU8.subarray(ptr, ptr + size), c.off);
          p.consts = { chunk: c, off: c.off };
          c.off += STEP;
      };
      g.submit = (id, serial) => {
          const cmd = g.objs[id];
          const q = g.device.queue;
          for (const c of cmd.chunks) q.writeBuffer(c.buf, 0, c.data, 0, c.off);
          q.submit([cmd.enc.finish()]);
          g.del(id);
          g.submitted = serial;
          const chunks = cmd.chunks;
          q.onSubmittedWorkDone().then(() => {
              if (globalThis.nhalGpu !== g) return;
              if (serial > g.completed) g.completed = serial;
              for (const c of chunks) g.pool.push(c);
          });
      };
      g.drop = (id) => { // nie abgeschickt: Blöcke sofort zurück
          const cmd = g.objs[id];
          if (cmd) for (const c of cmd.chunks) g.pool.push(c);
          g.del(id);
      };

      if (typeof navigator === "undefined" || !navigator.gpu) { g.state = -1; g.reason = 1; return; }
      navigator.gpu.requestAdapter().then((a) => {
          if (!a) { g.state = -1; g.reason = 2; return; }
          const info = a.info || {};
          g.name = [info.vendor, info.architecture, info.description].filter((x) => x).join(" ") || "WebGPU";
          const features = [];
          if (a.features.has("timestamp-query")) features.push("timestamp-query");
          if (a.features.has("indirect-first-instance")) features.push("indirect-first-instance");
          return a.requestDevice({ requiredFeatures: features }).then((d) => {
              if (globalThis.nhalGpu !== g) { d.destroy(); return; }
              g.device = d;
              g.ts = d.features.has("timestamp-query");
              g.fi = d.features.has("indirect-first-instance");
              d.addEventListener("uncapturederror", (e) => g.errors.push(String(e.error && e.error.message)));
              d.lost.then((l) => {
                  if (g.device !== d || l.reason === "destroyed") return;
                  g.state = -1; g.reason = 3; g.errors.push("GPU-Gerät verloren: " + l.message);
              });
              g.state = 1;
          });
      }).catch((e) => { g.state = -1; g.reason = 4; g.errors.push(String(e)); });
  },
});
