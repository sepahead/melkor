function showEntryError(error) {
  const message = "The viewer application could not start. Reload the viewer or restore its application files.";
  const overlay = document.getElementById("overlay");
  const title = document.getElementById("ov-title");
  const retry = document.getElementById("ov-retry");
  for (const id of [
    "app", "canvas-description", "hud", "scenes", "bar", "tl", "dropTarget", "helpDialog",
  ]) {
    document.getElementById(id).inert = true;
  }
  document.getElementById("app").setAttribute("aria-busy", "false");
  title.textContent = message;
  title.className = "title err";
  overlay.classList.add("show", "error");
  overlay.setAttribute("role", "alertdialog");
  overlay.setAttribute("aria-live", "assertive");
  overlay.setAttribute("aria-modal", "true");
  retry.hidden = false;
  retry.textContent = "Reload";
  retry.addEventListener("click", () => location.reload(), { once: true });
  window.__viewer = {
    state: { ready: true, loading: false, rendered: false, error: message },
    getStats() { return { ...this.state }; },
  };
  queueMicrotask(() => retry.focus({ preventScroll: true }));
  console.error(message, error);
}

import("./viewer.js").catch(showEntryError);
