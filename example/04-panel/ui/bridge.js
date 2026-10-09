const output = root.querySelector('[data-output]')

const write = (value) => {
  if (!output) return
  output.textContent = typeof value === 'string'
    ? value
    : JSON.stringify(value, null, 2)
}

const call = async (method, params) => {
  try {
    write(await xbase.call(method, params))
  } catch (error) {
    write(String(error))
  }
}

const handlers = {
  schema: () => call('panel.schema'),
  get: () => call('panel.get', { id: 'panelsample.scale' }),
  set: () => xbase.set('panelsample.scale', 2.5).then(write),
  getText: () => xbase.getText('panelsample.title').then(write),
  setText: () => xbase.setText('panelsample.title', '来自 Custom 脚本').then(write),
  run: () => xbase.run('panelsample.printLog').then(write),
  rect: () => call('panel.rect'),
  setSize: () => call('panel.setSize', { width: 720, height: 560 }),
  setPos: () => call('panel.setPos', { x: 80, y: 80 }),
  hide: () => call('panel.hide'),
}

const buttons = root.querySelectorAll('[data-method]')
buttons.forEach((button) => {
  button.addEventListener('click', () => handlers[button.dataset.method]?.())
})

const unsubscribe = xbase.on('panel.changed', (payload) => {
  write({ event: 'panel.changed', payload })
})

xbase.on('panel.textChanged', (payload) => {
  write({ event: 'panel.textChanged', payload })
})

return () => {
  buttons.forEach((button) => {
    button.replaceWith(button.cloneNode(true))
  })
  if (typeof unsubscribe === 'function') unsubscribe()
}
