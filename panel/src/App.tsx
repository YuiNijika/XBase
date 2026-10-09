import { useCallback, useEffect, useMemo, useState } from 'react'
import './App.css'
import { Shell } from '@/components/shell'
import { SectionList } from '@/components/section'
import { Toaster } from '@/components/ui/sonner'
import { callQuiet, componentBindings, fetchSchema, isBridgeAvailable, on, readText, readValue, type PanelMod, type PanelSchema } from '@/lib/bridge'

export default function App() {
  const [schema, setSchema] = useState<PanelSchema | null>(null)
  const [error, setError] = useState('')
  const [activeModId, setActiveModId] = useState('')
  const [activePageId, setActivePageId] = useState('')
  const [values, setValues] = useState<Record<string, number>>({})
  const [textValues, setTextValues] = useState<Record<string, string>>({})

  useEffect(() => {
    if (!isBridgeAvailable()) {
      setError('网页桥接未就位，面板拿不到宿主的界面')
      return
    }

    let alive = true
    fetchSchema()
      .then((next) => {
        if (!alive) return
        setSchema(next)
        setActiveModId(next.activeModId || next.mods[0]?.id || '')
      })
      .catch(() => {
        if (alive) setError('读取界面注册表失败')
      })
    return () => {
      alive = false
    }
  }, [])

  const activeMod = useMemo<PanelMod | null>(
    () => schema?.mods.find((mod) => mod.id === activeModId) ?? schema?.mods[0] ?? null,
    [schema, activeModId],
  )

  // 模组变了就把页面重置到第一页，否则会停在上一个模组的页面上
  useEffect(() => {
    setActivePageId(activeMod?.pages[0]?.id ?? '')
  }, [activeMod?.id])

  const activePage = activeMod?.pages.find((page) => page.id === activePageId) ?? activeMod?.pages[0] ?? null

  const controlIds = useMemo(() => {
    if (!activePage) return [] as string[]
    const controls = activePage.sections
      .flatMap((section) => section.controls)
    return [...new Set([...controls
      .filter((control) => ['toggle', 'float', 'int', 'select', 'color', 'progress', 'radio', 'multiselect'].includes(control.kind))
      .map((control) => control.id),
      ...controls.flatMap((control) => componentBindings(control.component)).filter((binding) => binding.kind === 0).map((binding) => binding.controlId)])]
  }, [activePage])

  const textControlIds = useMemo(() => {
    if (!activePage) return [] as string[]
    const controls = activePage.sections
      .flatMap((section) => section.controls)
    return [...new Set([...controls
      .filter((control) => control.kind === 'text' || control.kind === 'textarea')
      .map((control) => control.id),
      ...controls.flatMap((control) => componentBindings(control.component)).filter((binding) => binding.kind === 1 || binding.kind === 3).map((binding) => binding.controlId)])]
  }, [activePage])

  // 初始值向宿主取一次，避免开关显示与真实状态相反
  useEffect(() => {
    if (controlIds.length === 0) return
    let alive = true
    const pending = controlIds.map(async (id) => {
      try {
        return [id, await readValue(id)] as const
      } catch {
        return [id, undefined] as const
      }
    })
    void Promise.all(pending).then((entries) => {
      if (!alive) return
      const next: Record<string, number> = {}
      for (const [id, value] of entries) {
        if (value !== undefined) next[id] = value
      }
      setValues(next)
    })
    return () => {
      alive = false
    }
  }, [controlIds])

  useEffect(() => {
    if (textControlIds.length === 0) return
    let alive = true
    void Promise.all(textControlIds.map(async (id) => [id, await readText(id).catch(() => undefined)] as const)).then((entries) => {
      if (!alive) return
      const next: Record<string, string> = {}
      for (const [id, value] of entries) {
        if (value !== undefined) next[id] = value
      }
      setTextValues(next)
    })
    return () => {
      alive = false
    }
  }, [textControlIds])

  // 模组自己在游戏里改了状态时推事件过来，显示跟着走
  useEffect(() => {
    on('panel.changed', (payload) => {
      const detail = payload as { id?: string; value?: number } | null
      if (!detail?.id || typeof detail.value !== 'number') return
      setValues((previous) => ({ ...previous, [detail.id as string]: detail.value as number }))
    })
    on('panel.textChanged', (payload) => {
      const detail = payload as { id?: string; value?: string } | null
      if (!detail?.id || typeof detail.value !== 'string') return
      setTextValues((previous) => ({ ...previous, [detail.id as string]: detail.value as string }))
    })
  }, [])

  const onValue = useCallback((id: string, next: number) => {
    setValues((previous) => ({ ...previous, [id]: next }))
  }, [])

  const onTextValue = useCallback((id: string, next: string) => {
    setTextValues((previous) => ({ ...previous, [id]: next }))
  }, [])

  if (error) {
    return (
      <div className="flex h-screen w-screen items-center justify-center bg-background p-8 text-sm text-muted-foreground">
        {error}
      </div>
    )
  }

  if (!schema) {
    return (
      <div className="flex h-screen w-screen items-center justify-center bg-background p-8 text-sm text-muted-foreground">
        正在读取界面注册表
      </div>
    )
  }

  if (schema.mods.length === 0) {
    return (
      <div className="flex h-screen w-screen items-center justify-center overflow-hidden bg-background p-8 text-foreground">
        <div className="w-full max-w-xl text-center">
          <div className="text-xs font-medium uppercase tracking-[0.28em] text-muted-foreground">XBase Panel</div>
          <h1 className="mt-4 text-3xl font-semibold tracking-tight">等待模组挂载</h1>
          <p className="mx-auto mt-3 max-w-md text-sm leading-6 text-muted-foreground">
            当前没有可用的面板界面。挂载 XBase 模组后，这里会显示统一的控制面板。
          </p>
          <div className="mt-8 flex items-center justify-center gap-3">
            <button
              type="button"
              className="rounded-md border border-border bg-card px-4 py-2 text-sm font-medium text-foreground transition-colors hover:bg-accent"
              onClick={() => window.location.reload()}
            >
              重新读取
            </button>
            <button
              type="button"
              className="rounded-md bg-primary px-4 py-2 text-sm font-medium text-primary-foreground transition-colors hover:opacity-90"
              onClick={() => void callQuiet('panel.hide')}
            >
              关闭面板
            </button>
          </div>
        </div>
      </div>
    )
  }

  return (
    <Shell
      mods={schema.mods}
      activeMod={activeMod}
      activePageId={activePage?.id ?? ''}
      onSelectMod={setActiveModId}
      onSelectPage={setActivePageId}
      onClose={() => void callQuiet('panel.hide')}
    >
      <Toaster />
      {activeMod && activePage ? (
        <SectionList
          sections={activePage.sections}
          values={values}
          textValues={textValues}
          onValue={onValue}
          onTextValue={onTextValue}
        />
      ) : (
        <p className="text-sm text-muted-foreground">没有模组挂载界面</p>
      )}
    </Shell>
  )
}
