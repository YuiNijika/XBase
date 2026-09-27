import { useEffect, useState } from 'react'
import { cn } from '@/lib/cn'
import { runAction, writeValue, type PanelControl, type PanelSection } from '@/lib/bridge'

type SectionListProps = {
  sections: PanelSection[]
  values: Record<string, number>
  onValue: (id: string, next: number) => void
}

// 按宿主下发的分区依次渲染。能力门控与版本筛选宿主已经做过，
// 这里只处理界面内部的显隐依赖
export function SectionList({ sections, values, onValue }: SectionListProps) {
  if (sections.length === 0) {
    return <p className="text-sm text-muted-foreground">这个模组没有登记任何界面</p>
  }

  return (
    <div className="grid gap-6">
      {sections.map((section) => (
        <Section
          key={section.id}
          section={section}
          values={values}
          onValue={onValue}
        />
      ))}
    </div>
  )
}

function Section({ section, values, onValue }: { section: PanelSection; values: Record<string, number>; onValue: (id: string, next: number) => void }) {
  const controls = section.controls.filter((control) => visible(control, values))
  if (controls.length === 0) return null

  const columns = section.columns > 1 ? section.columns : 1
  const inline = section.inline && columns > 1

  return (
    <div className="grid gap-3">
      <div className="h-px bg-border/60" />
      <div className="text-sm font-medium">{section.label}</div>
      {section.hint ? <p className="text-xs text-muted-foreground">{section.hint}</p> : null}
      {inline ? (
        <div className="flex flex-wrap gap-2">
          {controls.map((control) => (
            <ControlRow
              key={control.id}
              control={control}
              disabled={!section.enabled}
              value={values[control.id]}
              onValue={onValue}
            />
          ))}
        </div>
      ) : (
        <div className="grid gap-3" style={{ gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))` }}>
          {controls.map((control) => (
            <ControlRow
              key={control.id}
              control={control}
              disabled={!section.enabled}
              value={values[control.id]}
              onValue={onValue}
            />
          ))}
        </div>
      )}
    </div>
  )
}

// 取不到被依赖项的值时先显示，避免加载期间控件闪进闪出
function visible(control: PanelControl, values: Record<string, number>): boolean {
  const expression = control.visibleWhen
  if (!expression) return true

  const inverted = expression.startsWith('!')
  const dependsOn = inverted ? expression.slice(1) : expression
  const current = values[dependsOn]
  if (current === undefined) return true
  const on = current !== 0
  return inverted ? !on : on
}

function ControlRow({
  control,
  disabled,
  value,
  onValue,
}: {
  control: PanelControl
  disabled: boolean
  value: number | undefined
  onValue: (id: string, next: number) => void
}) {
  if (control.kind === 'toggle') {
    const checked = value !== undefined && value !== 0
    return (
      <label className="flex items-center justify-between gap-3 text-sm">
        <span className="min-w-0 truncate">{control.label}</span>
        <Switch
          checked={checked}
          disabled={disabled || !control.enabled}
          onCheckedChange={(next) => {
            onValue(control.id, next ? 1 : 0)
            void writeValue(control.id, next)
          }}
        />
      </label>
    )
  }

  if (control.kind === 'select') {
    return (
      <label className="flex items-center justify-between gap-3 text-sm">
        <span className="min-w-0 truncate">{control.label}</span>
        <select
          className="h-8 min-w-0 max-w-[60%] rounded-md border border-input bg-background px-2 text-sm text-foreground disabled:opacity-50"
          value={String(Math.round(value ?? 0))}
          disabled={disabled || !control.enabled}
          onChange={(event) => {
            const next = Number(event.currentTarget.value)
            onValue(control.id, next)
            void writeValue(control.id, next)
          }}
        >
          {(control.options ?? []).map((option, index) => (
            <option key={option.value} value={String(index)}>
              {option.label}
            </option>
          ))}
        </select>
      </label>
    )
  }

  if (control.kind === 'float' || control.kind === 'int') {
    return (
      <NumberRow control={control} disabled={disabled || !control.enabled} value={value} onValue={onValue} />
    )
  }

  return (
    <button
      type="button"
      disabled={disabled || !control.enabled}
      onClick={() => void runAction(control.id)}
      className={cn(
        'h-8 rounded-md border border-border bg-background px-3 text-sm text-foreground',
        'transition-[background-color,color] duration-150',
        'hover:bg-muted disabled:opacity-50 active:scale-[0.98]',
        'focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none',
      )}
    >
      {control.label}
    </button>
  )
}

function NumberRow({
  control,
  disabled,
  value,
  onValue,
}: {
  control: PanelControl
  disabled: boolean
  value: number | undefined
  onValue: (id: string, next: number) => void
}) {
  const [text, setText] = useState(value === undefined ? '' : String(value))

  useEffect(() => {
    if (value !== undefined) setText(String(value))
  }, [value])

  const bounded = control.bounded && control.max > control.min

  // 有边界的走拖动条：拖动时只更新本地数值，松手才提交宿主，来回拖不会被回写打断
  if (bounded) {
    const step = control.step > 0 ? control.step : control.kind === 'int' ? 1 : 0.05
    const current = value === undefined || Number.isNaN(value) ? control.min : Math.min(control.max, Math.max(control.min, value))
    return (
      <div className="text-sm">
        <div className="flex items-center justify-between gap-3">
          <span className="min-w-0 truncate">{control.label}</span>
          <span className="shrink-0 tabular-nums text-muted-foreground">{formatValue(control, current)}</span>
        </div>
        <input
          type="range"
          className="mt-1.5 w-full cursor-pointer disabled:opacity-50"
          style={{ accentColor: 'oklch(0.72 0.14 var(--accent-hue))' }}
          min={control.min}
          max={control.max}
          step={step}
          value={current}
          disabled={disabled}
          aria-label={control.label}
          onChange={(event) => onValue(control.id, Number(event.currentTarget.value))}
          onPointerUp={() => void writeValue(control.id, current)}
          onKeyUp={() => void writeValue(control.id, current)}
        />
      </div>
    )
  }

  // 没有边界的数值才用输入框
  return (
    <div className="flex items-center gap-2 text-sm">
      <span className="min-w-0 flex-1 truncate">{control.label}</span>
      <input
        className="h-8 w-20 rounded-md border border-input bg-background px-2 text-sm text-foreground disabled:opacity-50"
        inputMode={control.kind === 'int' ? 'numeric' : 'decimal'}
        value={text}
        disabled={disabled}
        aria-label={control.label}
        onChange={(event) => setText(event.currentTarget.value)}
      />
      <button
        type="button"
        disabled={disabled}
        onClick={() => {
          const next = Number(text)
          if (Number.isNaN(next)) return
          onValue(control.id, next)
          void writeValue(control.id, next)
        }}
        className={cn(
          'h-8 shrink-0 rounded-md border border-border bg-background px-3 text-sm',
          'hover:bg-muted disabled:opacity-50 active:scale-[0.98]',
        )}
      >
        应用
      </button>
    </div>
  )
}

function Switch({
  checked,
  disabled,
  onCheckedChange,
}: {
  checked: boolean
  disabled: boolean
  onCheckedChange: (next: boolean) => void
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={() => onCheckedChange(!checked)}
      className={cn(
        'relative inline-flex h-5 w-9 shrink-0 items-center rounded-full border border-transparent',
        'transition-colors duration-150 disabled:opacity-50',
        'focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none',
        checked ? 'bg-primary' : 'bg-input',
      )}
    >
      <span
        className={cn(
          'pointer-events-none block size-4 rounded-full bg-background shadow-sm',
          'transition-transform duration-150',
          checked ? 'translate-x-[18px]' : 'translate-x-[2px]',
        )}
      />
    </button>
  )
}

// 按宿主给的 format 渲染数值（%.1f 这类），没有就按类型给默认
function formatValue(control: PanelControl, value: number): string {
  const format = control.format
  if (!format) {
    return control.kind === 'int' ? String(Math.round(value)) : value.toFixed(2)
  }
  const token = /%(\.\d+)?f/.exec(format)
  if (!token) return format
  const rendered = token[1] ? value.toFixed(Number(token[1].slice(1))) : String(Math.round(value))
  return format.replace(token[0], rendered)
}
