import { validateLocalFileInProcess } from "./local-file-validator.js";

self.postMessage({ kind: "ready" });

let active = null;
self.addEventListener("message", async (event) => {
  const request = event.data ?? {};
  if (request.kind === "cancel") {
    if (active?.id === request.id) active.canceled = true;
    return;
  }
  if (request.kind !== "validate") return;
  if (active) active.canceled = true;
  const operation = { canceled: false, id: request.id };
  active = operation;
  try {
    const { file, localFileTypes } = request;
    if (!Array.isArray(localFileTypes)) {
      throw new Error("The local file type table is invalid.");
    }
    const descriptor = await validateLocalFileInProcess(
      file,
      new Map(localFileTypes),
      () => operation.canceled,
    );
    if (!operation.canceled) {
      self.postMessage({ descriptor, id: operation.id, kind: "result", ok: true });
    }
  } catch (error) {
    if (operation.canceled) return;
    const message = error instanceof Error && error.message.length > 0
      ? error.message
      : "The local file check failed safely.";
    self.postMessage({ id: operation.id, kind: "result", message, ok: false });
  } finally {
    if (active === operation) active = null;
  }
});
