import { useEffect, useRef, useState } from 'react'
import { cn } from '@/lib/cn'
import { call } from '@/lib/bridge'
import { useAppearance, type ThemeMode } from '@/lib/appearance'
import type { PanelMod, PanelPage } from '@/lib/bridge'

type ShellProps = {
  mods: PanelMod[]
  activeMod: PanelMod | null
  activePageId: string
  onSelectMod: (id: string) => void
  onSelectPage: (id: string) => void
  onClose: () => void
  children: React.ReactNode
}

type DragState = {
  x: number
  y: number
  width: number
  height: number
}

const THEME_LABELS: Record<ThemeMode, string> = {
  system: '跟随系统',
  light: '浅色',
  dark: '深色',
}

// 侧边栏是挂载进来的模组，内容区独立滚动，右下角可拖动改面板尺寸
export function Shell({ mods, activeMod, activePageId, onSelectMod, onSelectPage, onClose, children }: ShellProps) {
  const scrollRef = useRef<HTMLDivElement | null>(null)
  const dragRef = useRef<DragState | null>(null)
  const moveRef = useRef<{ x: number; y: number; left: number; top: number } | null>(null)
  const rectRef = useRef<{ x: number; y: number } | null>(null)
  const [resizing, setResizing] = useState(false)
  const [moving, setMoving] = useState(false)

  // 面板矩形提前取好并定期刷新，拖动时才不用等异步回调
  useEffect(() => {
    const refresh = () => {
      call<{ x: number; y: number }>('panel.rect')
        .then((rect) => {
          rectRef.current = { x: rect.x, y: rect.y }
        })
        .catch(() => {
          rectRef.current = null
        })
    }
    refresh()
    const timer = window.setInterval(refresh, 2000)
    return () => window.clearInterval(timer)
  }, [])

  // 换模组后内容区回到顶部，翻长列表时不会停在上一处
  useEffect(() => {
    scrollRef.current?.scrollTo({ top: 0 })
  }, [activeMod?.id, activePageId])

  const sendSize = (width: number, height: number) => {
    void call('panel.setSize', { width, height })
  }

  const sendPosition = (x: number, y: number) => {
    void call('panel.setPos', { x, y })
  }

  const beginMove = (event: React.PointerEvent<HTMLDivElement>) => {
    const start = rectRef.current
    if (!start) {
      // 缓存还没就绪，先补一次再让用户重拖，避免异步里丢指针捕获
      void call<{ x: number; y: number }>('panel.rect').then((rect) => {
        rectRef.current = { x: rect.x, y: rect.y }
      })
      return
    }
    event.currentTarget.setPointerCapture(event.pointerId)
    moveRef.current = { x: event.clientX, y: event.clientY, left: start.x, top: start.y }
    setMoving(true)
  }

  const moveMove = (event: React.PointerEvent<HTMLDivElement>) => {
    const start = moveRef.current
    if (!start) return
    sendPosition(start.left + (event.clientX - start.x), start.top + (event.clientY - start.y))
  }

  const endMove = (event: React.PointerEvent<HTMLDivElement>) => {
    const start = moveRef.current
    moveRef.current = null
    setMoving(false)
    if (!start) return
    sendPosition(start.left + (event.clientX - start.x), start.top + (event.clientY - start.y))
  }

  const beginResize = (event: React.PointerEvent<HTMLDivElement>) => {
    event.preventDefault()
    event.currentTarget.setPointerCapture(event.pointerId)
    dragRef.current = { x: event.clientX, y: event.clientY, width: window.innerWidth, height: window.innerHeight }
    setResizing(true)
  }

  const moveResize = (event: React.PointerEvent<HTMLDivElement>) => {
    const start = dragRef.current
    if (!start) return
    sendSize(start.width + (event.clientX - start.x), start.height + (event.clientY - start.y))
  }

  const endResize = (event: React.PointerEvent<HTMLDivElement>) => {
    const start = dragRef.current
    dragRef.current = null
    setResizing(false)
    if (!start) return
    sendSize(start.width + (event.clientX - start.x), start.height + (event.clientY - start.y))
  }

  // 拖拽需要键盘等价物，聚焦手柄后用方向键调尺寸
  const resizeWithKeyboard = (event: React.KeyboardEvent<HTMLDivElement>) => {
    const step = 40
    const width = window.innerWidth + (event.key === 'ArrowRight' ? step : event.key === 'ArrowLeft' ? -step : 0)
    const height = window.innerHeight + (event.key === 'ArrowUp' ? step : event.key === 'ArrowDown' ? -step : 0)
    if (width === window.innerWidth && height === window.innerHeight) return
    event.preventDefault()
    sendSize(width, height)
  }

  const pages: PanelPage[] = activeMod?.pages ?? []

  return (
    <div className={cn('relative flex h-screen w-screen overflow-hidden bg-background text-foreground', resizing && 'select-none')}>
      <aside className="flex w-[76px] shrink-0 flex-col border-r border-border/60 bg-sidebar/80 backdrop-blur-xl">
        <div className="flex h-14 items-center justify-center border-b border-border/60 px-2">
          <span className="truncate text-sm font-semibold tracking-tight">XBase</span>
        </div>
        <nav aria-label="已挂载的模组" className="mt-2 flex w-full min-h-0 flex-1 flex-col items-center gap-1 overflow-y-auto px-1.5 pb-2">
          {mods.map((mod) => {
            const isActive = mod.id === activeMod?.id
            return (
              <button
                key={mod.id}
                type="button"
                title={mod.title}
                aria-current={isActive ? 'page' : undefined}
                onClick={() => onSelectMod(mod.id)}
                className={cn(
                  'flex w-full flex-col items-center justify-center gap-[3px] rounded-xl py-2',
                  'transition-[color,background-color,transform] duration-150',
                  'focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none',
                  'active:scale-[0.97]',
                  isActive ? 'bg-primary text-primary-foreground' : 'text-muted-foreground hover:bg-muted hover:text-foreground',
                )}
              >
                <span
                  className={cn(
                    'flex size-6 items-center justify-center rounded-md text-[11px]',
                    isActive ? 'bg-primary-foreground/20' : 'bg-muted',
                  )}
                  aria-hidden="true"
                >
                  {monogram(mod.title || mod.id)}
                </span>
                <span className={cn('w-full truncate text-center text-[11px] leading-none', isActive ? 'font-semibold' : 'font-medium')}>
                  {mod.title || mod.id}
                </span>
              </button>
            )
          })}
        </nav>
        <div className="border-t border-border/60 px-1 py-2 text-center text-[10px] leading-tight text-muted-foreground">
          <div className="truncate opacity-70">{mods.length} 个模组</div>
        </div>
      </aside>

      <main className="flex min-h-0 min-w-0 flex-1 flex-col">
        <header className="flex items-center justify-between border-b border-border/60 px-7 py-5">
          <div
            className={cn('min-w-0 flex-1 select-none', moving ? 'cursor-grabbing' : 'cursor-move')}
            title="拖动移动面板"
            onPointerDown={beginMove}
            onPointerMove={moveMove}
            onPointerUp={endMove}
            onPointerCancel={endMove}
          >
            <h1 className="text-xl font-semibold tracking-tight">{activeMod?.title ?? 'XBase Panel'}</h1>
            <p className="mt-1 text-sm text-muted-foreground">{activeMod?.subtitle ?? '没有模组挂载界面'}</p>
          </div>
          <div className="flex items-center gap-1.5">
            {activeMod?.version ? (
              <span className="rounded-full bg-secondary px-2 py-0.5 text-[11px] text-secondary-foreground">
                {activeMod.version}
              </span>
            ) : null}
            <PaletteMenu />
            <IconButton label="关闭" onClick={onClose}>
              <path d="M5 5l14 14M19 5L5 19" />
            </IconButton>
          </div>
        </header>

        {pages.length > 1 ? (
          <div className="flex gap-1 border-b border-border/60 px-7 py-2">
            {pages.map((page) => (
              <button
                key={page.id}
                type="button"
                onClick={() => onSelectPage(page.id)}
                className={cn(
                  'h-7 rounded-md px-3 text-[13px] transition-colors duration-150',
                  page.id === activePageId
                    ? 'bg-primary text-primary-foreground'
                    : 'text-muted-foreground hover:bg-muted hover:text-foreground',
                )}
              >
                {page.label}
              </button>
            ))}
          </div>
        ) : null}

        <div ref={scrollRef} className="min-h-0 flex-1 overflow-y-auto">
          <div className="px-7 py-6">{children}</div>
        </div>
      </main>

      <div
        role="separator"
        aria-label="拖动调整面板尺寸"
        title="拖动调整面板尺寸"
        onPointerDown={beginResize}
        onPointerMove={moveResize}
        onPointerUp={endResize}
        onPointerCancel={endResize}
        onKeyDown={resizeWithKeyboard}
        tabIndex={0}
        className={cn(
          'group absolute right-0 bottom-0 z-40 flex h-8 w-8 cursor-nwse-resize items-end justify-end rounded-tl-lg p-1.5',
          'transition-[background-color,color] duration-150',
          'focus-visible:outline-none focus-visible:bg-primary/15 focus-visible:text-foreground',
          resizing ? 'bg-primary/15 text-foreground' : 'hover:bg-muted/60',
        )}
      >
        <span
          className={cn(
            'pointer-events-none block h-3 w-3 border-r-2 border-b-2',
            resizing ? 'border-primary' : 'border-muted-foreground/60 group-hover:border-foreground',
          )}
        />
      </div>
    </div>
  )
}

// 标题栏调色板：明暗 + 自定义色相，只改 --accent-hue 一个变量
function PaletteMenu() {
  const { theme, hue, setTheme, setHue } = useAppearance()
  const [open, setOpen] = useState(false)

  return (
    <div className="relative">
      <IconButton label="外观" onClick={() => setOpen(!open)}>
        <path d="M12 3a9 9 0 100 18 2.5 2.5 0 000-5 2 2 0 010-4 4 4 0 000-4 2.5 2.5 0 000-5z" />
      </IconButton>
      {open ? (
        <div className="absolute right-0 z-50 mt-2 w-56 rounded-xl border border-border bg-popover p-3 text-popover-foreground shadow-lg">
          <div className="text-[11px] text-muted-foreground">主题</div>
          <div className="mt-2 grid gap-1">
            {(Object.keys(THEME_LABELS) as ThemeMode[]).map((mode) => (
              <button
                key={mode}
                type="button"
                onClick={() => setTheme(mode)}
                className={cn(
                  'flex h-7 items-center justify-between rounded-md px-2 text-[13px]',
                  theme === mode ? 'bg-primary/15 text-foreground' : 'hover:bg-muted',
                )}
              >
                {THEME_LABELS[mode]}
                {theme === mode ? <span className="text-[11px] text-muted-foreground">当前</span> : null}
              </button>
            ))}
          </div>
          <div className="mt-3 text-[11px] text-muted-foreground">强调色色相</div>
          <input
            type="range"
            min={0}
            max={359}
            step={1}
            value={hue}
            aria-label="强调色色相"
            onChange={(event) => setHue(Number(event.currentTarget.value))}
            className="mt-1.5 w-full cursor-pointer"
            style={{ accentColor: 'oklch(0.72 0.14 var(--accent-hue))' }}
          />
        </div>
      ) : null}
    </div>
  )
}

function IconButton({ label, onClick, children }: { label: string; onClick: () => void; children: React.ReactNode }) {
  return (
    <button
      type="button"
      title={label}
      aria-label={label}
      onClick={onClick}
      className={cn(
        'flex size-8 items-center justify-center rounded-md border border-border bg-background',
        'transition-colors duration-150 hover:bg-muted active:scale-[0.97]',
        'focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none',
      )}
    >
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" className="size-4" aria-hidden="true">
        {children}
      </svg>
    </button>
  )
}

// 模组名可能只有一个字符，也要显示得出来，所以按码点取而不是按字节
function monogram(title: string): string {
  const first = [...title][0]
  return first ? first.toUpperCase() : '?'
}
