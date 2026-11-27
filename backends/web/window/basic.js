// window.basic – Web: DOM-Teil der Fenster (Canvas, Listener, Textfeld, Drops).
// Als --js-library gelinkt (über js_libraries im Katalog), weil der Code für
// EM_JS zu groß ist. Aufruf aus C: nhal_web_window_create() in basic.c.
addToLibrary({
  nhal_web_window_create__deps: ["$UTF8ToString", "$lengthBytesUTF8", "$FS"],
  nhal_web_window_create: function (id, selector, title, w, h, flags) {
      const g = globalThis.nhalWin;
      if (typeof document === "undefined") return -1;
      let canvas, created = false;
      if (selector) {
          canvas = document.querySelector(UTF8ToString(selector));
          if (!canvas) return -2;
      } else {
          canvas = document.createElement("canvas");
          canvas.id = "nhal-window-" + id;
          document.body.appendChild(canvas);
          created = true;
      }
      if (title) document.title = UTF8ToString(title);
      const ac = new AbortController();
      const opt = { signal: ac.signal };
      const text = document.createElement("textarea");
      text.setAttribute("autocomplete", "off");
      text.setAttribute("autocapitalize", "off");
      text.setAttribute("spellcheck", "false");
      text.style.cssText = "position:fixed;left:-1000px;top:0;width:1px;height:1px;opacity:0;";
      document.body.appendChild(text);
      canvas.tabIndex = 0;
      if (created || !canvas.style.width) {
          canvas.style.width = w + "px";
          canvas.style.height = h + "px";
      }
      if (flags & 2) canvas.style.display = "none";
      const win = { id: id, canvas: canvas, text: text, created: created, textInput: true, highDpi: (flags & 8) !== 0,
                    w: w, h: h, composing: false, lastClick: { t: 0, x: 0, y: 0, n: 0 } };
      const now = () => performance.now() / 1000;
      const mods = (e) => (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0) |
                          (e.getModifierState && e.getModifierState("CapsLock") ? 16 : 0);
      const put = (t, time, ints, floats, s) => g.q.push({ t: t, w: id, time: time, i: ints || [], f: floats || [], s: s });
      const pos = (e) => { const r = canvas.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
      win.size = () => {
          const cw = canvas.clientWidth || win.w, ch = canvas.clientHeight || win.h;
          const scale = win.highDpi ? (globalThis.devicePixelRatio || 1) : 1;
          return [cw, ch, Math.round(cw * scale), Math.round(ch * scale), scale];
      };
      const target = () => (win.textInput ? text : canvas);
      win.focus = () => target().focus({ preventScroll: true });

      const onKey = (e, down) => {
          const key = g.index[e.code] || 0;
          put(down ? 6 : 7, e.timeStamp / 1000, [key, mods(e), down && e.repeat ? 1 : 0, key]);
          const printable = e.key && e.key.length === 1 && !e.ctrlKey && !e.metaKey;
          if (down && printable && win.textInput && !e.isTrusted) put(8, e.timeStamp / 1000, [-1, 0], [], e.key);
          const nav = ["Tab", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "PageUp", "PageDown", "Home", "End"];
          if (nav.includes(e.code) || (!win.textInput && (printable || e.code === "Backspace"))) e.preventDefault();
      };
      for (const el of [canvas, text]) {
          el.addEventListener("keydown", (e) => onKey(e, true), opt);
          el.addEventListener("keyup", (e) => onKey(e, false), opt);
          el.addEventListener("focus", (e) => put(4, e.timeStamp / 1000), opt);
          el.addEventListener("blur", (e) => {
              if (document.activeElement !== canvas && document.activeElement !== text) put(5, e.timeStamp / 1000);
          }, opt);
      }
      text.addEventListener("compositionstart", () => { win.composing = true; }, opt);
      text.addEventListener("compositionupdate", (e) => {
          const n = lengthBytesUTF8(e.data || "");
          put(9, e.timeStamp / 1000, [n, 0], [], e.data || "");
      }, opt);
      text.addEventListener("compositionend", (e) => {
          win.composing = false;
          if (e.data) put(8, e.timeStamp / 1000, [-1, 0], [], e.data);
          text.value = "";
      }, opt);
      text.addEventListener("input", (e) => {
          if (win.composing || e.isComposing) return;
          if (e.data && e.inputType === "insertText") put(8, e.timeStamp / 1000, [-1, 0], [], e.data);
          text.value = "";
      }, opt);

      const buttonOf = [0, 2, 1, 3, 4];
      const pointer = (e, phase) => {
          const p = pos(e);
          const t = e.timeStamp / 1000;
          if (e.pointerType === "touch") {
              put(17, t, [e.pointerId, phase], [p[0], p[1], e.pressure || (phase === 2 ? 0 : 1)]);
              return;
          }
          if (e.pointerType === "pen") {
              const contact = (e.buttons & 1) !== 0 || e.pressure > 0;
              put(18, t, [e.pointerId, phase, (e.buttons & 32) ? 1 : 0, contact ? 1 : 0, e.buttons >>> 0],
                  [p[0], p[1], e.pressure || 0, (e.tiltX || 0) / 90, (e.tiltY || 0) / 90]);
          }
          if (phase === 1) {
              put(10, t, [0, e.buttons & 31, 0, mods(e)], [p[0], p[1], e.movementX || 0, e.movementY || 0]);
          } else if (phase === 0 || phase === 2) {
              const b = buttonOf[e.button] !== undefined ? buttonOf[e.button] : 0;
              let clicks = 1;
              if (phase === 0) {
                  const c = win.lastClick;
                  const near = Math.abs(p[0] - c.x) < 4 && Math.abs(p[1] - c.y) < 4;
                  clicks = (t - c.t < 0.5 && near) ? c.n + 1 : 1;
                  win.lastClick = { t: t, x: p[0], y: p[1], n: clicks };
              }
              put(phase === 0 ? 11 : 12, t, [b, e.buttons & 31, phase === 0 ? clicks : 0, mods(e)], [p[0], p[1], 0, 0]);
          }
      };
      canvas.addEventListener("pointerdown", (e) => {
          if (e.pointerType !== "touch") e.preventDefault();
          win.focus();
          if (canvas.setPointerCapture) { try { canvas.setPointerCapture(e.pointerId); } catch (x) {} }
          pointer(e, 0);
      }, opt);
      canvas.addEventListener("pointermove", (e) => pointer(e, 1), opt);
      canvas.addEventListener("pointerup", (e) => pointer(e, 2), opt);
      canvas.addEventListener("pointercancel", (e) => {
          if (e.pointerType === "touch") { const p = pos(e); put(17, e.timeStamp / 1000, [e.pointerId, 3], [p[0], p[1], 0]); }
      }, opt);
      canvas.addEventListener("pointerenter", (e) => { if (e.pointerType === "mouse") { const p = pos(e); put(14, e.timeStamp / 1000, [], [p[0], p[1]]); } }, opt);
      canvas.addEventListener("pointerleave", (e) => { if (e.pointerType === "mouse") { const p = pos(e); put(15, e.timeStamp / 1000, [], [p[0], p[1]]); } }, opt);
      canvas.addEventListener("contextmenu", (e) => e.preventDefault(), opt);
      canvas.style.touchAction = "none";
      canvas.addEventListener("wheel", (e) => {
          e.preventDefault();
          const p = pos(e);
          const k = e.deltaMode === 1 ? 1 : e.deltaMode === 2 ? 10 : 1;
          put(13, e.timeStamp / 1000, [e.deltaMode === 0 ? 1 : 0, mods(e)], [-e.deltaX * k, -e.deltaY * k, p[0], p[1]]);
      }, { passive: false, signal: ac.signal });
      canvas.addEventListener("dragover", (e) => e.preventDefault(), opt);
      canvas.addEventListener("drop", (e) => {
          e.preventDefault();
          const files = Array.from((e.dataTransfer && e.dataTransfer.files) || []);
          const p = pos(e);
          try { FS.mkdirTree("/drop"); } catch (x) {}
          files.forEach((f, i) => {
              f.arrayBuffer().then((buf) => {
                  let path = "/drop/" + f.name.replace(/[\/\\]/g, "_");
                  for (let n = 1; FS.analyzePath(path).exists; ++n) path = "/drop/" + n + "-" + f.name.replace(/[\/\\]/g, "_");
                  FS.writeFile(path, new Uint8Array(buf));
                  put(16, now(), [i, files.length], [p[0], p[1]], path);
              });
          });
      }, opt);
      let lastSize = "";
      const ro = typeof ResizeObserver !== "undefined" ? new ResizeObserver(() => {
          const s = win.size();
          const key = s.join(",");
          if (key === lastSize) return;
          lastSize = key;
          put(3, now(), [s[0], s[1], s[2], s[3]], [s[4]]);
      }) : null;
      if (ro) ro.observe(canvas);
      win.destroy = () => {
          ac.abort();
          if (ro) ro.disconnect();
          text.remove();
          if (created) canvas.remove();
          delete g.wins[id];
      };
      g.wins[id] = win;
      return 0;
  },
});
