export type ComponentNode = {
  component: string
  text?: string
  textPath?: string
  props?: Record<string, unknown> | null
  children?: ComponentNode[] | null
  slots?: Record<string, ComponentNode[]> | null
  templates?: Record<string, ComponentNode> | null
  bindings?: ComponentBinding[] | null
}

export type ComponentBinding = {
  controlId: string
  property: string
  event: string
  kind: 0 | 1 | 2 | 3
}

export function componentBindings(node?: ComponentNode): ComponentBinding[] {
  if (!node) return []
  return [
    ...(node.bindings ?? []),
    ...(node.children ?? []).flatMap(componentBindings),
    ...Object.values(node.slots ?? {}).flatMap((nodes) => nodes.flatMap(componentBindings)),
    ...Object.values(node.templates ?? {}).flatMap(componentBindings),
  ]
}

export type PanelControl = {
  id: string
  kind: 'toggle' | 'float' | 'int' | 'action' | 'select' | 'text' | 'textarea' | 'color' | 'progress' | 'custom' | 'radio' | 'multiselect' | 'heading' | 'separator' | 'component'
  component?: ComponentNode
  label: string
  hint: string
  // 能力不支持时为假，控件照画但置灰，玩家能看到这个功能确实存在
  enabled: boolean
  bounded: boolean
  min: number
  max: number
  step: number
  format: string
  text: string
  placeholder: string
  html: string
  script: string
  style: string
  // Custom 资源的相对路径。运行时会先读取文件并填充上面的内联字段，
  // 这里保留路径方便调试、工具链展示以及兼容旧版 schema。
  htmlFile: string
  scriptFile: string
  styleFile: string
  readOnly: boolean
  // 依赖同一分区里另一个控件，值为真才显示，前置叹号取反
  visibleWhen: string
  options?: { value: string; label: string }[]
}

export type PanelSection = {
  id: string
  label: string
  hint: string
  columns: number
  inline: boolean
  enabled: boolean
  controls: PanelControl[]
}

export type PanelPage = {
  id: string
  label: string
  sections: PanelSection[]
}

export type PanelMod = {
  id: string
  title: string
  subtitle: string
  version: string
  pages: PanelPage[]
}

export type PanelSchema = {
  game: string
  gameName: string
  version: string
  activeModId: string
  mods: PanelMod[]
}

type XBaseBridge = {
  call: <T = unknown>(method: string, params?: Record<string, unknown>) => Promise<T>
  on: (event: string, callback: (payload: unknown) => void) => void
}

declare global {
  interface Window {
    xbase?: XBaseBridge
  }
}

export function isBridgeAvailable(): boolean {
  return typeof window.xbase !== 'undefined'
}

export async function call<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T> {
  if (!window.xbase) {
    throw new Error('bridge offline')
  }
  return window.xbase.call<T>(method, params)
}

export function on(event: string, callback: (payload: unknown) => void): void {
  window.xbase?.on(event, callback)
}

// 失败不打断界面，调用方只关心有没有拿到值
export async function callQuiet(method: string, params?: Record<string, unknown>): Promise<void> {
  try {
    await call(method, params)
  } catch {
    /* 面板离线或方法不存在都当作无事发生 */
  }
}

export async function fetchSchema(): Promise<PanelSchema> {
  return call<PanelSchema>('panel.schema')
}

export async function readValue(id: string): Promise<number | undefined> {
  const result = await call<{ ok?: boolean; value?: unknown }>('panel.get', { id })
  return typeof result?.value === 'number' ? result.value : undefined
}

export async function readText(id: string): Promise<string | undefined> {
  const result = await call<{ ok?: boolean; value?: unknown }>('panel.getText', { id })
  return typeof result?.value === 'string' ? result.value : undefined
}

export async function writeValue(id: string, value: number | boolean): Promise<void> {
  await callQuiet('panel.set', { id, value })
}

export async function writeText(id: string, value: string): Promise<void> {
  await callQuiet('panel.setText', { id, value })
}

export async function runAction(id: string): Promise<void> {
  await callQuiet('panel.run', { id })
}
