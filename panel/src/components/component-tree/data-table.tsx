import { useState } from 'react'
import {
  flexRender, getCoreRowModel, getFilteredRowModel, getPaginationRowModel,
  getSortedRowModel, useReactTable, type ColumnDef, type SortingState,
} from '@tanstack/react-table'
import { ArrowDown, ArrowUp, ArrowUpDown, ChevronLeft, ChevronRight } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'

interface Props {
  columns: { key: string; label: string }[]
  data: Record<string, unknown>[]
  pageSize?: number
  disabled?: boolean
  onRowClick?: (row: Record<string, unknown>) => void
}

export function DataTable({ columns, data, pageSize = 10, disabled, onRowClick }: Props) {
  const [sorting, setSorting] = useState<SortingState>([])
  const [filter, setFilter] = useState('')
  const definitions: ColumnDef<Record<string, unknown>>[] = columns.map(({ key, label }) => ({
    accessorKey: key,
    header: label,
    cell: ({ getValue }) => String(getValue() ?? ''),
  }))
  const table = useReactTable({
    columns: definitions,
    data,
    state: { sorting, globalFilter: filter },
    onSortingChange: setSorting,
    onGlobalFilterChange: setFilter,
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
    getFilteredRowModel: getFilteredRowModel(),
    getPaginationRowModel: getPaginationRowModel(),
    initialState: { pagination: { pageSize: Math.max(1, pageSize) } },
  })
  return (
    <div className="grid min-w-0 gap-3">
      <Input aria-label="筛选表格" placeholder="筛选…" value={filter} disabled={disabled} onChange={(event) => setFilter(event.target.value)} />
      <Table>
        <TableHeader>{table.getHeaderGroups().map((group) => <TableRow key={group.id}>{group.headers.map((header) => {
          const sorted = header.column.getIsSorted()
          const Icon = sorted === 'asc' ? ArrowUp : sorted === 'desc' ? ArrowDown : ArrowUpDown
          return <TableHead key={header.id} aria-sort={sorted === 'asc' ? 'ascending' : sorted === 'desc' ? 'descending' : 'none'}>
            <Button variant="ghost" disabled={disabled} onClick={header.column.getToggleSortingHandler()}>{flexRender(header.column.columnDef.header, header.getContext())}<Icon aria-hidden="true" /></Button>
          </TableHead>
        })}</TableRow>)}</TableHeader>
        <TableBody>{table.getRowModel().rows.map((row) => <TableRow key={row.id}>{row.getVisibleCells().map((cell, index) => <TableCell key={cell.id}>{index === 0 && onRowClick ? <Button variant="ghost" disabled={disabled} onClick={() => onRowClick(row.original)}>{flexRender(cell.column.columnDef.cell, cell.getContext())}</Button> : flexRender(cell.column.columnDef.cell, cell.getContext())}</TableCell>)}</TableRow>)}
          {!table.getRowModel().rows.length && <TableRow><TableCell colSpan={columns.length} className="text-center text-muted-foreground">没有结果</TableCell></TableRow>}
        </TableBody>
      </Table>
      <div className="flex items-center justify-end gap-2">
        <span className="text-sm tabular-nums">{table.getState().pagination.pageIndex + 1} / {Math.max(1, table.getPageCount())}</span>
        <Button size="icon" variant="outline" aria-label="上一页" title="上一页" disabled={disabled || !table.getCanPreviousPage()} onClick={() => table.previousPage()}><ChevronLeft /></Button>
        <Button size="icon" variant="outline" aria-label="下一页" title="下一页" disabled={disabled || !table.getCanNextPage()} onClick={() => table.nextPage()}><ChevronRight /></Button>
      </div>
    </div>
  )
}
