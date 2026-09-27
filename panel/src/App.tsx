import { useCallback, useEffect, useMemo, useState } from 'react'
import './App.css'
import { Shell } from '@/components/shell'
import { SectionList } from '@/components/section'
import { callQuiet, fetchSchema, isBridgeAvailable, on, readValue, type PanelMod, type PanelSchema } from '@/lib/bridge'

export default function App() {
  const [schema, setSchema] = useState<PanelSchema | null>(null)
  const [error, setError] = useState('')
  const [activeModId, setActiveModId] = useState('')
  const [activePageId, setActivePageId] = useState('')
  const [values, setValues] = useState<Record<string, number>>({})

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
    return activePage.sections
      .flatMap((section) => section.controls)
      .filter((control) => control.kind !== 'action')
      .map((control) => control.id)
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

  // 模组自己在游戏里改了状态时推事件过来，显示跟着走
  useEffect(() => {
    on('panel.changed', (payload) => {
      const detail = payload as { id?: string; value?: number } | null
      if (!detail?.id || typeof detail.value !== 'number') return
      setValues((previous) => ({ ...previous, [detail.id as string]: detail.value as number }))
    })
  }, [])

  const onValue = useCallback((id: string, next: number) => {
    setValues((previous) => ({ ...previous, [id]: next }))
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

  return (
    <Shell
      mods={schema.mods}
      activeMod={activeMod}
      activePageId={activePage?.id ?? ''}
      onSelectMod={setActiveModId}
      onSelectPage={setActivePageId}
      onClose={() => void callQuiet('panel.hide')}
    >
      {activeMod && activePage ? (
        <SectionList sections={activePage.sections} values={values} onValue={onValue} />
      ) : (
        <p className="text-sm text-muted-foreground">没有模组挂载界面</p>
      )}
    </Shell>
  )
}
