export function closeModalById(id: string) {
  const root = document.getElementById(id);
  root?.querySelector<HTMLButtonElement>('button[data-bs-dismiss="modal"]')?.click();
}

export function showModalById(id: string) {
  const modalRoot = document.getElementById(id);
  const modalCtor = (
    window as Window & { bootstrap?: { Modal?: { getOrCreateInstance: (el: Element) => { show: () => void } } } }
  ).bootstrap?.Modal;
  if (!modalRoot || !modalCtor?.getOrCreateInstance) return;
  modalCtor.getOrCreateInstance(modalRoot).show();
}
