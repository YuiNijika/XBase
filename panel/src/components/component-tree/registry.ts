import type { ElementType } from 'react'
import {
  Area, AreaChart, Bar, BarChart, CartesianGrid, Cell, ComposedChart,
  Legend, Line, LineChart, Pie, PieChart, PolarAngleAxis, PolarGrid,
  PolarRadiusAxis, Radar, RadarChart, RadialBar, RadialBarChart,
  ReferenceArea, ReferenceDot, ReferenceLine, ResponsiveContainer,
  Scatter, ScatterChart, Tooltip as RechartsTooltip, XAxis, YAxis, ZAxis,
} from 'recharts'
import { DatePicker } from './date-picker'
import { DataTable } from './data-table'
import { ToastButton } from './toast'
import { Form, FormField } from './form'
import { Slider } from './slider'
import { SonnerToaster } from './sonner'
import { Toaster as BaseToaster } from '@/components/ui/toast'

const modules = import.meta.glob<Record<string, unknown>>('../ui/*.tsx', { eager: true })
const registry: Record<string, ElementType> = {}

for (const module of Object.values(modules)) {
  for (const [name, component] of Object.entries(module)) {
    if (!/^[A-Z]/.test(name)) continue
    if (typeof component !== 'function' && !(typeof component === 'object' && component !== null && '$$typeof' in component)) continue
    registry[name] = component as ElementType
  }
}

Object.assign(registry, {
  Area, AreaChart, Bar, BarChart, CartesianGrid, Cell, ComposedChart,
  Legend, Line, LineChart, Pie, PieChart, PolarAngleAxis, PolarGrid,
  PolarRadiusAxis, Radar, RadarChart, RadialBar, RadialBarChart,
  ReferenceArea, ReferenceDot, ReferenceLine, ResponsiveContainer,
  Scatter, ScatterChart, RechartsTooltip, XAxis, YAxis, ZAxis,
  DatePicker, DataTable, ToastButton, Form, FormField, Slider, SonnerToaster, BaseToaster,
  Toaster: SonnerToaster,
})

const elements = ['div', 'span', 'p', 'h1', 'h2', 'h3', 'section', 'header', 'footer', 'main', 'nav', 'form', 'fieldset', 'legend', 'ul', 'ol', 'li', 'a', 'img', 'strong', 'em', 'code', 'pre', 'br', 'option', 'optgroup'] as const
for (const element of elements) registry[element] = element

export function resolveComponent(name: string): ElementType | undefined {
  return registry[name]
}

export function supportedComponents(): string[] {
  return Object.keys(registry).sort()
}
