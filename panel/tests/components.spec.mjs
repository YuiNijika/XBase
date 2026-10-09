import { test, expect } from '@playwright/test'

const node = (component, props = {}, children = [], extra = {}) => ({ component, props, children, ...extra })
const text = (component, value, props = {}) => node(component, props, [], { text: value })
const binding = (controlId, property, event, kind = 0) => ({ controlId, property, event, kind })
const component = (id, root) => ({ id, kind: 'component', component: root, enabled: true, readOnly: false })

const fixtures = [
  {
    id: 'legacy.toggle', kind: 'toggle', label: '旧版开关', enabled: true,
  },
  {
    id: 'legacy.slider', kind: 'int', label: '旧版滑条', enabled: true,
    bounded: true, min: 0, max: 100, step: 1, format: '',
  },
  component('tree.switch', node('Switch', { 'aria-label': '新开关' }, [], {
    bindings: [binding('test.switch', 'checked', 'onCheckedChange')],
  })),
  component('tree.input', node('Input', { 'aria-label': '输入名字', defaultValue: '' }, [], {
    bindings: [binding('test.name', 'value', 'onChange', 1)],
  })),
  component('tree.select', node('Select', { defaultValue: 'vc' }, [
    node('SelectTrigger', { 'aria-label': '选择游戏' }, [node('SelectValue')]),
    node('SelectContent', {}, [text('SelectItem', 'Vice City', { value: 'vc' }), text('SelectItem', 'San Andreas', { value: 'sa' })]),
  ], { bindings: [binding('test.game', 'value', 'onValueChange', 1)] })),
  component('tree.dialog', node('Dialog', {}, [
    node('DialogTrigger', { asChild: true }, [text('Button', '打开对话框')]),
    node('DialogContent', {}, [
      node('DialogHeader', {}, [text('DialogTitle', '操作确认'), text('DialogDescription', '确认执行操作')]),
      text('Button', '执行动作', {},),
    ]),
  ])),
  component('tree.tabs', node('Tabs', { defaultValue: 'first' }, [
    node('TabsList', {}, [text('TabsTrigger', '第一项', { value: 'first' }), text('TabsTrigger', '第二项', { value: 'second' })]),
    text('TabsContent', '第一项内容', { value: 'first' }),
    text('TabsContent', '第二项内容', { value: 'second' }),
  ])),
  component('tree.form', node('Form', { defaultValues: { name: '' } }, [
    node('FormField', { name: 'name', rules: { required: '请填写名字' } }, [
      node('FormItem', {}, [text('FormLabel', '表单名字'), node('FormControl', {}, [node('Input')]), node('FormMessage')]),
    ]),
    text('Button', '提交表单', { type: 'submit' }),
  ], { bindings: [binding('test.form', '', 'onSubmit', 3)] })),
  component('tree.date', node('DatePicker', { placeholder: '选择日期' }, [], {
    bindings: [binding('test.date', 'value', 'onValueChange', 1)],
  })),
  component('tree.table', node('DataTable', {
    columns: [{ key: 'name', label: '名称' }, { key: 'amount', label: '数值' }],
    data: [{ name: 'Alpha', amount: 2 }, { name: 'Beta', amount: 1 }, { name: 'Gamma', amount: 3 }],
    pageSize: 2,
  })),
  component('tree.chart', node('ChartContainer', {
    config: { count: { label: '数量', color: '#16a34a' } },
    className: 'h-[200px] w-full',
  }, [
    node('BarChart', { data: [{ name: 'A', count: 4 }, { name: 'B', count: 8 }] }, [
      node('XAxis', { dataKey: 'name' }),
      node('Bar', { dataKey: 'count', fill: 'var(--color-count)', isAnimationActive: false }),
      node('ChartTooltip', {}, [], { slots: { content: [node('ChartTooltipContent')] } }),
    ]),
  ])),
  component('tree.toast', node('BaseToaster', {}, [
    text('ToastButton', '显示通知', { title: '操作已完成', engine: 'base' }),
  ])),
  component('tree.disabled', node('Button', {}, [], {
    text: '禁用动作',
    bindings: [binding('test.disabled', '', 'onClick', 2)],
  })),
  component('tree.combobox', node('Combobox', { items: ['Apple', 'Banana'] }, [
    node('ComboboxInput', { 'aria-label': '水果' }),
    node('ComboboxContent', {}, [
      node('ComboboxList', {}, [], {
        templates: {
          children: node('ComboboxItem', { value: { $arg: '' } }, [], { textPath: '' }),
        },
      }),
      text('ComboboxEmpty', '没有结果'),
    ]),
  ], { bindings: [binding('test.fruit', 'value', 'onValueChange', 1)] })),
]

fixtures.find((item) => item.id === 'tree.dialog').component.children[1].children[1].bindings = [binding('test.action', '', 'onClick', 2)]
fixtures.find((item) => item.id === 'tree.disabled').enabled = false
fixtures.find((item) => item.id === 'tree.combobox').component.children[1].children[0].templates.children.textPath = '$value'

test.beforeEach(async ({ page }) => {
  await page.addInitScript(({ controls }) => {
    window.calls = []
    window.listeners = {}
    const values = { 'legacy.toggle': 0, 'legacy.slider': 25, 'test.switch': 0 }
    const texts = { 'test.name': '', 'test.game': 'vc', 'test.date': '2026-10-09', 'test.fruit': '' }
    window.xbase = {
      on: (name, callback) => { window.listeners[name] = callback },
      call: async (method, params = {}) => {
        window.calls.push({ method, params })
        if (method === 'panel.schema') return {
          game: 'vc', gameName: 'Vice City', version: 'v0.1.0-rc', activeModId: 'test',
          mods: [{
            id: 'test', title: '测试', subtitle: '', version: '1',
            pages: [{ id: 'main', label: '设置', sections: [{ id: 'controls', label: '设置', columns: 1, enabled: true, controls }] }],
          }],
        }
        if (method === 'panel.get') return { ok: true, value: values[params.id] }
        if (method === 'panel.getText') return { ok: true, value: texts[params.id] }
        if (method === 'panel.rect') return { x: 0, y: 0, width: 1000, height: 800 }
        if (method === 'panel.set') values[params.id] = params.value
        if (method === 'panel.setText') texts[params.id] = params.value
        return { ok: true }
      },
    }
  }, { controls: fixtures })
  await page.goto('/')
  await expect(page.getByRole('switch', { name: '旧版开关' })).toBeVisible()
  await expect(page.getByText('组件配置无效')).toHaveCount(0)
})

test('完整组件 registry 与旧控件兼容', async ({ page }) => {
  const names = await page.evaluate(async () => {
    const module = await import('/src/components/component-tree/registry.ts')
    return module.supportedComponents()
  })
  for (const name of ['Accordion', 'AlertDialog', 'Attachment', 'Bubble', 'Calendar', 'ChartContainer', 'Combobox', 'Command', 'ContextMenu', 'DataTable', 'DatePicker', 'Dialog', 'Drawer', 'Form', 'FormField', 'InputOTP', 'Marker', 'Menubar', 'Message', 'MessageScroller', 'NavigationMenu', 'Questionnaire', 'ResizablePanel', 'Sidebar', 'Toast', 'Tooltip', 'ToggleGroup']) {
    expect(names).toContain(name)
  }
  expect(names.length).toBeGreaterThan(250)
  await page.getByRole('switch', { name: '旧版开关' }).click()
  await page.getByRole('slider', { name: '旧版滑条' }).focus()
  await page.keyboard.press('ArrowRight')
  await expect.poll(() => page.evaluate(() => window.calls.some((item) => item.method === 'panel.set' && item.params.id === 'legacy.slider' && item.params.value === 26))).toBe(true)
})

test('数值 文本 单选 初始读取与推送', async ({ page }) => {
  await page.getByRole('switch', { name: '新开关' }).click()
  await page.getByRole('textbox', { name: '输入名字' }).fill('Tommy')
  await page.getByRole('combobox', { name: '选择游戏' }).click()
  await page.getByRole('option', { name: 'San Andreas' }).click()
  await expect.poll(() => page.evaluate(() => window.calls.some((item) => item.method === 'panel.setText' && item.params.id === 'test.game' && item.params.value === 'sa'))).toBe(true)
  await page.evaluate(() => window.listeners['panel.changed']({ id: 'test.switch', value: 0 }))
  await expect(page.getByRole('switch', { name: '新开关' })).not.toBeChecked()
  await expect(page.getByRole('button', { name: '禁用动作' })).toBeDisabled()
})

test('组合组件 弹层动作与键盘关闭', async ({ page }) => {
  await page.getByRole('button', { name: '打开对话框' }).click()
  await expect(page.getByRole('dialog', { name: '操作确认' })).toBeVisible()
  await page.getByRole('button', { name: '执行动作' }).click()
  await expect.poll(() => page.evaluate(() => window.calls.some((item) => item.method === 'panel.run' && item.params.id === 'test.action'))).toBe(true)
  await page.keyboard.press('Escape')
  await expect(page.getByRole('dialog')).not.toBeVisible()
  await page.getByRole('tab', { name: '第二项' }).click()
  await expect(page.getByText('第二项内容')).toBeVisible()
})

test('表单校验与 JSON 提交', async ({ page }) => {
  await page.getByRole('button', { name: '提交表单' }).click()
  await expect(page.getByText('请填写名字')).toBeVisible()
  await page.getByRole('textbox', { name: '表单名字' }).fill('Claude')
  await page.getByRole('button', { name: '提交表单' }).click()
  await expect.poll(() => page.evaluate(() => window.calls.some((item) => item.method === 'panel.setText' && item.params.id === 'test.form' && JSON.parse(item.params.value).name === 'Claude'))).toBe(true)
})

test('表格筛选与通知', async ({ page }) => {
  await page.getByRole('textbox', { name: '筛选表格' }).fill('Gamma')
  await expect(page.getByRole('cell', { name: 'Gamma' })).toBeVisible()
  await expect(page.getByRole('cell', { name: 'Alpha' })).not.toBeVisible()
  await page.getByRole('button', { name: '显示通知' }).click()
  await expect(page.getByText('操作已完成')).toBeVisible()
})

test('日期与模板集合', async ({ page }) => {
  await page.getByRole('button', { name: '2026/10/9' }).click()
  const day = page.locator('[data-day]').filter({ hasText: /^15$/ }).first()
  await day.click()
  await expect.poll(() => page.evaluate(() => window.calls.some((item) => item.method === 'panel.setText' && item.params.id === 'test.date' && item.params.value === '2026-10-15'))).toBe(true)
  await page.keyboard.press('Escape')
  await page.getByRole('combobox', { name: '水果' }).fill('Ban')
  await page.getByRole('option', { name: 'Banana' }).click()
})

test('图表和桌面窄屏视觉检查', async ({ page }) => {
  await expect(page.locator('.recharts-bar-rectangle')).toHaveCount(2)
  for (const [width, height] of [[1280, 900], [390, 844]]) {
    await page.setViewportSize({ width, height })
    await page.getByRole('textbox', { name: '输入名字' }).scrollIntoViewIfNeeded()
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true)
    await page.screenshot({ path: `test-results/panel-${width}.png`, fullPage: true })
  }
})
