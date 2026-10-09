import { cloneElement, Component, createElement, Fragment, isValidElement, type ReactElement, type ReactNode } from 'react'
import { runAction, writeText, writeValue, type ComponentNode } from '@/lib/bridge'
import { resolveComponent } from './registry'
import { TooltipProvider } from '@/components/ui/tooltip'

export interface TreeProps {
  node: ComponentNode
  disabled: boolean
  values: Record<string, number>
  textValues: Record<string, string>
  onValue: (id: string, value: number) => void
  onTextValue: (id: string, value: string) => void
}

function eventValue(value: unknown): unknown {
  if (typeof value !== 'object' || value === null) return value
  if ('currentTarget' in value) {
    const target = value.currentTarget
    if (target instanceof HTMLInputElement && (target.type === 'checkbox' || target.type === 'radio')) return target.checked
    if (target instanceof HTMLInputElement || target instanceof HTMLTextAreaElement || target instanceof HTMLSelectElement) return target.value
    const result: Record<string, unknown> = {}
    for (const key of ['type', 'key', 'code', 'button', 'buttons', 'clientX', 'clientY', 'altKey', 'ctrlKey', 'metaKey', 'shiftKey']) {
      if (key in value) result[key] = (value as Record<string, unknown>)[key]
    }
    return result
  }
  return value
}

function dateValue(value: unknown): unknown {
  if (typeof value === 'string') {
    const date = new Date(/^\d{4}-\d{2}-\d{2}$/.test(value) ? `${value}T00:00:00` : value)
    return Number.isNaN(date.getTime()) ? undefined : date
  }
  if (Array.isArray(value)) return value.map(dateValue)
  if (typeof value === 'object' && value !== null) {
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, dateValue(item)]))
  }
  return value
}

function cleanProps(input: Record<string, unknown> | null | undefined): Record<string, unknown> {
  return Object.fromEntries(Object.entries(input ?? {}).filter(([key, value]) => {
    if (/^on[A-Z]/.test(key) || ['dangerouslySetInnerHTML', 'ref', '__proto__', 'constructor', 'children'].includes(key)) return false
    if (['href', 'src', 'action', 'formAction'].includes(key) && typeof value === 'string') {
      return !/^\s*(javascript|vbscript|data):/i.test(value)
    }
    return true
  }))
}

function argumentPath(scope: unknown, path: string): unknown {
  if (path === '$value') return scope
  let value = scope
  for (const part of path.split('.').filter(Boolean)) {
    if (['__proto__', 'constructor', 'prototype'].includes(part)) return undefined
    if (typeof value !== 'object' || value === null) return undefined
    value = (value as Record<string, unknown>)[part]
  }
  return value
}

function resolveArguments(value: unknown, scope: unknown): unknown {
  if (Array.isArray(value)) return value.map((item) => resolveArguments(item, scope))
  if (typeof value !== 'object' || value === null) return value
  if ('$arg' in value && typeof value.$arg === 'string') return argumentPath(scope, value.$arg)
  return Object.fromEntries(Object.entries(value).map(([name, item]) => [name, resolveArguments(item, scope)]))
}

function renderNode(node: ComponentNode, context: TreeProps, path: string, scope?: unknown): ReactNode {
  const text = node.textPath ? String(argumentPath(scope, node.textPath) ?? '') : node.text
  if (!node.component) return text ?? null
  const component = resolveComponent(node.component)
  if (!component) return <p key={path} role="alert" className="text-sm text-destructive">未知组件：{node.component}</p>
  const props = cleanProps(resolveArguments(node.props, scope) as Record<string, unknown> | undefined)
  const disabled = context.disabled || props.disabled === true
  const childContext = disabled ? { ...context, disabled: true } : context
  if (disabled) props.disabled = true
  if (node.component === 'Button' && props.type === undefined) props.type = 'button'

  for (const binding of node.bindings ?? []) {
    if (binding.kind !== 2 && binding.property) {
      const source = binding.kind === 0 ? context.values[binding.controlId] : context.textValues[binding.controlId]
      const defaults: Record<string, string> = { value: 'defaultValue', checked: 'defaultChecked', open: 'defaultOpen', pressed: 'defaultPressed' }
      const defaultProperty = defaults[binding.property]
      const value = source ?? props[binding.property] ?? props[defaultProperty] ?? (binding.kind === 0 ? 0 : binding.kind === 1 ? '' : undefined)
      if (defaultProperty) delete props[defaultProperty]
      if (value !== undefined) {
        if (binding.kind === 3 && typeof value === 'string') {
          try {
            props[binding.property] = JSON.parse(value)
          } catch {
            props[binding.property] = node.props?.[binding.property]
          }
        } else if (['checked', 'pressed', 'open'].includes(binding.property)) {
          props[binding.property] = Boolean(value) && value !== '0'
        } else if (node.component === 'Slider' && binding.property === 'value' && binding.kind === 0) {
          props.value = [Number(value)]
        } else {
          props[binding.property] = value
        }
      }
    }
    if (!/^on[A-Z]/.test(binding.event)) continue
    const previous = props[binding.event]
    props[binding.event] = (...args: unknown[]) => {
      if (disabled) return
      if (binding.event === 'onSubmit' && typeof args[0] === 'object' && args[0] !== null && 'preventDefault' in args[0]) {
        (args[0] as { preventDefault: () => void }).preventDefault()
      }
      if (typeof previous === 'function') previous(...args)
      if (binding.kind === 2) {
        void runAction(binding.controlId)
        return
      }
      const value = eventValue(args[0])
      if (binding.kind === 0) {
        const next = typeof value === 'boolean' ? Number(value) : Number(Array.isArray(value) ? value[0] : value)
        if (!Number.isFinite(next)) return
        context.onValue(binding.controlId, next)
        void writeValue(binding.controlId, next)
        return
      }
      let next: string | undefined
      try {
        next = binding.kind === 3 ? JSON.stringify(value ?? null) : value instanceof Date ? localDate(value) : String(value ?? '')
      } catch {
        return
      }
      if (next === undefined) return
      context.onTextValue(binding.controlId, next)
      void writeText(binding.controlId, next)
    }
  }

  if (node.component === 'Calendar') {
    for (const key of ['selected', 'defaultMonth', 'month', 'startMonth', 'endMonth', 'today']) {
      if (props[key] !== undefined) props[key] = dateValue(props[key])
    }
  }
  for (const [name, nodes] of Object.entries(node.slots ?? {})) {
    const children = nodes.map((child, index) => renderNode(child, childContext, `${path}.${name}.${index}`, scope))
    props[name] = children.length === 1 ? children[0] : createElement(Fragment, {}, children)
  }
  for (const [name, template] of Object.entries(node.templates ?? {})) {
    if (/^on[A-Z]/.test(name) || ['__proto__', 'constructor', 'ref', 'dangerouslySetInnerHTML'].includes(name)) continue
    props[name] = (argument: unknown) => {
      const child = renderNode(template, childContext, `${path}.${name}`, argument)
      if (name === 'render' && isValidElement(child) && typeof argument === 'object' && argument !== null) {
        const element = child as ReactElement<Record<string, unknown>>
        return cloneElement(element, { ...argument, ...element.props })
      }
      return child
    }
  }
  props.key = path
  const children = (node.children ?? []).map((child, index) => renderNode(child, childContext, `${path}.${index}`, scope))
  if (text) children.unshift(text)
  return children.length ? createElement(component, props, ...children) : createElement(component, props)
}

export function localDate(date: Date): string {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`
}

class Boundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false }
  static getDerivedStateFromError(): { failed: boolean } {
    return { failed: true }
  }
  render(): ReactNode {
    if (this.state.failed) return <p role="alert" className="text-sm text-destructive">组件配置无效</p>
    return this.props.children
  }
}

export function ComponentTree(props: TreeProps) {
  return (
    <Boundary key={JSON.stringify(props.node)}>
      <TooltipProvider>
        {renderNode(props.node, props, 'root')}
      </TooltipProvider>
    </Boundary>
  )
}
