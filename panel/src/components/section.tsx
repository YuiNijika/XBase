import { useEffect, useRef, useState } from 'react'
import { cn } from '@/lib/cn'
import { call, runAction, writeText, writeValue, type PanelControl, type PanelSection } from '@/lib/bridge'
import { ComponentTree } from '@/components/component-tree'
import { Switch } from '@/components/ui/switch'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Textarea } from '@/components/ui/textarea'
import { NativeSelect, NativeSelectOption } from '@/components/ui/native-select'
import { Checkbox } from '@/components/ui/checkbox'
import { RadioGroup, RadioGroupItem } from '@/components/ui/radio-group'
import { Progress } from '@/components/ui/progress'
import { Slider } from '@/components/ui/slider'
import { Separator } from '@/components/ui/separator'

type SectionListProps = {
  sections: PanelSection[]
  values: Record<string, number>
  textValues: Record<string, string>
  onValue: (id: string, next: number) => void
  onTextValue: (id: string, next: string) => void
}

// 按宿主下发的分区依次渲染。能力门控与版本筛选宿主已经做过，
// 这里只处理界面内部的显隐依赖
export function SectionList({ sections, values, textValues, onValue, onTextValue }: SectionListProps) {
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
          textValues={textValues}
          onValue={onValue}
          onTextValue={onTextValue}
        />
      ))}
    </div>
  )
}

function Section({
  section,
  values,
  textValues,
  onValue,
  onTextValue,
}: {
  section: PanelSection
  values: Record<string, number>
  textValues: Record<string, string>
  onValue: (id: string, next: number) => void
  onTextValue: (id: string, next: string) => void
}) {
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
              textValue={textValues[control.id]}
              values={values}
              textValues={textValues}
              onValue={onValue}
              onTextValue={onTextValue}
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
              textValue={textValues[control.id]}
              values={values}
              textValues={textValues}
              onValue={onValue}
              onTextValue={onTextValue}
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
  textValue,
  values,
  textValues,
  onValue,
  onTextValue,
}: {
  control: PanelControl
  disabled: boolean
  value: number | undefined
  textValue: string | undefined
  values: Record<string, number>
  textValues: Record<string, string>
  onValue: (id: string, next: number) => void
  onTextValue: (id: string, next: string) => void
}) {
  if (control.kind === 'component' && control.component) {
    return <ComponentTree node={control.component} disabled={disabled || !control.enabled || control.readOnly} values={values} textValues={textValues} onValue={onValue} onTextValue={onTextValue} />
  }
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
        <NativeSelect
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
            <NativeSelectOption key={option.value} value={String(index)}>
              {option.label}
            </NativeSelectOption>
          ))}
        </NativeSelect>
      </label>
    )
  }

  if (control.kind === 'float' || control.kind === 'int') {
    return (
      <NumberRow control={control} disabled={disabled || !control.enabled} value={value} onValue={onValue} />
    )
  }

  if (control.kind === 'text' || control.kind === 'textarea') {
    return (
      <TextRow
        control={control}
        disabled={disabled || !control.enabled}
        value={textValue ?? ''}
        onValue={onTextValue}
      />
    )
  }

  if (control.kind === 'color') {
    return (
      <ColorRow
        control={control}
        disabled={disabled || !control.enabled}
        value={value}
        onValue={onValue}
      />
    )
  }

  if (control.kind === 'radio') {
    return <RadioRow control={control} disabled={disabled || !control.enabled} value={value} onValue={onValue} />
  }

  if (control.kind === 'multiselect') {
    return <MultiSelectRow control={control} disabled={disabled || !control.enabled} value={value} onValue={onValue} />
  }

  if (control.kind === 'progress') {
    return <ProgressRow control={control} value={value} />
  }

  if (control.kind === 'heading') {
    return (
      <div className="grid gap-1">
        <div className="text-sm font-semibold">{control.label}</div>
        {control.hint ? <div className="text-xs text-muted-foreground">{control.hint}</div> : null}
      </div>
    )
  }

  if (control.kind === 'separator') {
    return <Separator />
  }

  if (control.kind === 'custom') {
    return <CustomControl control={control} disabled={disabled || !control.enabled} />
  }

  return (
    <Button
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
    </Button>
  )
}

function TextRow({
  control,
  disabled,
  value,
  onValue,
}: {
  control: PanelControl
  disabled: boolean
  value: string
  onValue: (id: string, next: string) => void
}) {
  const [text, setText] = useState(value)

  useEffect(() => setText(value), [value])

  const commit = (next: string) => {
    onValue(control.id, next)
    void writeText(control.id, next)
  }

  if (control.readOnly) {
    return (
      <div className="grid gap-1 text-sm">
        <span className="font-medium">{control.label}</span>
        <div className="min-h-8 rounded-md border border-border bg-muted/30 px-2 py-1.5 whitespace-pre-wrap text-muted-foreground">
          {value || control.text || control.placeholder}
        </div>
      </div>
    )
  }

  if (control.kind === 'textarea') {
    return (
      <label className="grid gap-1 text-sm">
        <span className="font-medium">{control.label}</span>
        <Textarea
          className="min-h-24 resize-y rounded-md border border-input bg-background px-2 py-1.5 text-foreground disabled:opacity-50"
          value={text}
          placeholder={control.placeholder}
          disabled={disabled}
          onChange={(event) => setText(event.currentTarget.value)}
          onBlur={() => commit(text)}
        />
        {control.hint ? <span className="text-xs text-muted-foreground">{control.hint}</span> : null}
      </label>
    )
  }

  return (
    <label className="flex items-center gap-2 text-sm">
      <span className="min-w-0 flex-1 truncate">{control.label}</span>
      <Input
        className="h-8 min-w-0 flex-1 rounded-md border border-input bg-background px-2 text-sm text-foreground disabled:opacity-50"
        value={text}
        placeholder={control.placeholder}
        disabled={disabled}
        onChange={(event) => setText(event.currentTarget.value)}
        onBlur={() => commit(text)}
        onKeyDown={(event) => {
          if (event.key === 'Enter') commit(text)
        }}
      />
    </label>
  )
}

function ColorRow({
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
  const packed = Math.max(0, Math.round(value ?? 0))
  const hex = `#${(packed & 0xffffff).toString(16).padStart(6, '0')}`
  return (
    <label className="flex items-center justify-between gap-3 text-sm">
      <span className="min-w-0 truncate">{control.label}</span>
      <input
        type="color"
        className="h-8 w-12 cursor-pointer rounded-md border border-input bg-background p-0.5 disabled:opacity-50"
        value={hex}
        disabled={disabled || control.readOnly}
        onChange={(event) => {
          const next = Number.parseInt(event.currentTarget.value.slice(1), 16)
          onValue(control.id, next)
          void writeValue(control.id, next)
        }}
        aria-label={control.label}
      />
    </label>
  )
}

function RadioRow({
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
  const selected = Math.round(value ?? 0)
  return (
    <fieldset className="grid gap-1.5 text-sm">
      <legend className="font-medium">{control.label}</legend>
      <RadioGroup className="flex flex-wrap gap-2" value={String(selected)} disabled={disabled} onValueChange={(next) => {
        onValue(control.id, Number(next))
        void writeValue(control.id, Number(next))
      }}>
        {(control.options ?? []).map((option, index) => (
          <label key={option.value} className={cn('flex items-center gap-1.5', disabled && 'opacity-50')}>
            <RadioGroupItem
              value={String(index)}
              disabled={disabled}
            />
            <span>{option.label}</span>
          </label>
        ))}
      </RadioGroup>
    </fieldset>
  )
}

function MultiSelectRow({
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
  const mask = Math.max(0, Math.round(value ?? 0))
  return (
    <fieldset className="grid gap-1.5 text-sm">
      <legend className="font-medium">{control.label}</legend>
      <div className="grid gap-1.5 sm:grid-cols-2">
        {(control.options ?? []).map((option, index) => {
          const bit = 1 << index
          const checked = (mask & bit) !== 0
          return (
            <label key={option.value} className={cn('flex items-center gap-1.5', disabled && 'opacity-50')}>
              <Checkbox
                checked={checked}
                disabled={disabled}
                onCheckedChange={() => {
                  const next = checked ? mask & ~bit : mask | bit
                  onValue(control.id, next)
                  void writeValue(control.id, next)
                }}
              />
              <span>{option.label}</span>
            </label>
          )
        })}
      </div>
    </fieldset>
  )
}

function ProgressRow({ control, value }: { control: PanelControl; value: number | undefined }) {
  const min = control.min
  const max = control.max > min ? control.max : 1
  const current = Math.min(max, Math.max(min, value ?? min))
  const ratio = ((current - min) / (max - min)) * 100
  return (
    <div className="grid gap-1 text-sm">
      <div className="flex items-center justify-between gap-3">
        <span className="min-w-0 truncate">{control.label}</span>
        <span className="shrink-0 tabular-nums text-muted-foreground">{formatValue(control, current)}</span>
      </div>
      <Progress value={ratio} aria-label={control.label} />
    </div>
  )
}

function CustomControl({ control, disabled }: { control: PanelControl; disabled: boolean }) {
  const rootRef = useRef<HTMLDivElement | null>(null)

  useEffect(() => {
    const root = rootRef.current
    if (!root) return
    root.innerHTML = control.html || ''
    if (control.style) {
      const style = document.createElement('style')
      style.textContent = control.style
      root.prepend(style)
    }
    if (!control.script) return

    try {
      const cleanup = new Function('root', 'xbase', control.script)(
        root,
        {
          root,
          call,
          get: (id: string) => call('panel.get', { id }),
          set: (id: string, value: number | boolean) => call('panel.set', { id, value }),
          getText: (id: string) => call('panel.getText', { id }),
          setText: (id: string, value: string) => call('panel.setText', { id, value }),
          run: (id: string) => call('panel.run', { id }),
          on: (event: string, callback: (payload: unknown) => void) => {
            window.xbase?.on(event, callback)
          },
        },
      )
      return typeof cleanup === 'function' ? cleanup : undefined
    } catch (error) {
      console.error(`XBase custom panel control "${control.id}" failed`, error)
      return undefined
    }
  }, [control.html, control.script, control.style])

  return (
    <div
      className={cn(
        'min-h-8 rounded-md border border-border/70 bg-muted/20 p-3',
        disabled && 'pointer-events-none opacity-50',
      )}
      aria-label={control.label || undefined}
      ref={rootRef}
    />
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
        <Slider
          className="mt-1.5 w-full cursor-pointer disabled:opacity-50"
          style={{ accentColor: 'oklch(0.72 0.14 var(--accent-hue))' }}
          min={control.min}
          max={control.max}
          step={step}
          value={[current]}
          disabled={disabled}
          aria-label={control.label}
          onValueChange={([next]) => onValue(control.id, next)}
          onValueCommit={([next]) => void writeValue(control.id, next)}
        />
      </div>
    )
  }

  // 没有边界的数值才用输入框
  return (
    <div className="flex items-center gap-2 text-sm">
      <span className="min-w-0 flex-1 truncate">{control.label}</span>
      <Input
        className="h-8 w-20 rounded-md border border-input bg-background px-2 text-sm text-foreground disabled:opacity-50"
        inputMode={control.kind === 'int' ? 'numeric' : 'decimal'}
        value={text}
        disabled={disabled}
        aria-label={control.label}
        onChange={(event) => setText(event.currentTarget.value)}
      />
      <Button
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
      </Button>
    </div>
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
