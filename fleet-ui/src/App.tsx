import { useEffect, useState } from "react"
import { AudioLines, Bot, Boxes, Cable, ChevronRight, CircuitBoard, Copy, Image, Moon, RefreshCw, Sun } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { Empty, EmptyDescription, EmptyHeader, EmptyMedia, EmptyTitle } from "@/components/ui/empty"
import { Field, FieldGroup, FieldLabel } from "@/components/ui/field"
import { Input } from "@/components/ui/input"
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table"
import { useTheme } from "@/components/theme-provider"

type Device = { id: string; variant: string; version: string; target: string | null; state: string; seen: number; source: string; name: string | null; ota_seen: number | null }
type Asset = { id: number; name: string; size: number }
type Release = { tag: string; preview: boolean; url: string; assets: Asset[] }
type Tool = { name: string; description?: string; category?: string }
type Pack = { id: string; title: string; bytes: number; previews: {name: string; file: string; width: number; height: number}[] }
type Fleet = {
  devices: Device[]; releases: Release[]; repository: string; connected: boolean;
  music: { status?: string; title?: string }; tools: Tool[]; ota_url: string
  ports: { device: string; description: string }[]
}
const pages = [
  { title: "Thiết bị", icon: Bot },
  { title: "Bản phát hành", icon: Boxes },
  { title: "MCP & công cụ", icon: AudioLines },
  { title: "Giao diện", icon: Image },
]
const status: Record<string, string> = {
  unassigned: "Chưa gán", queued: "Chờ robot kiểm tra", offered: "Đã gửi bản cập nhật", installed: "Đã xác nhận cài",
}
const selectClass = "h-9 w-full rounded-md border bg-background px-2 text-sm focus-visible:ring-2 focus-visible:ring-ring"
async function request<T>(path: string, method = "GET", body?: object): Promise<T> {
  const response = await fetch(path, { method, cache: "no-store", signal: AbortSignal.timeout(65000),
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined })
  if (!response.ok) throw new Error((await response.text()).slice(0, 240))
  return response.json()
}

function ToolList({ tools }: { tools: Tool[] }) {
  const [query, setQuery] = useState("")
  const [category, setCategory] = useState("")
  const [index, setIndex] = useState(0)
  const categories = [...new Set(tools.flatMap(tool => tool.category ? [tool.category] : []))]
  const matches = tools.filter(tool => (!category || tool.category === category) &&
    (tool.name + " " + (tool.description ?? "")).toLocaleLowerCase().includes(query.trim().toLocaleLowerCase()))
  const pages = Math.max(1, Math.ceil(matches.length / 6))
  const current = Math.min(index, pages - 1)
  return <div className="flex flex-col gap-3">
    <div className="flex flex-wrap gap-2">
      <Input aria-label="Tìm công cụ" placeholder="Tìm tên hoặc mô tả…" className="min-w-32 flex-1" value={query} onChange={e => { setQuery(e.target.value); setIndex(0) }} />
      {categories.length > 0 && <select aria-label="Nhóm công cụ" className={selectClass + " w-auto"} value={category} onChange={e => { setCategory(e.target.value); setIndex(0) }}>
        <option value="">Tất cả nhóm</option>{categories.map(item => <option key={item}>{item}</option>)}
      </select>}
    </div>
    {matches.length ? <ul className="divide-y">{matches.slice(current * 6, current * 6 + 6).map(tool => <li key={tool.name}>
      <details className="py-2"><summary className="cursor-pointer break-all text-sm font-medium">{tool.name}</summary>
      {tool.description && <p className="mt-2 text-sm leading-relaxed text-muted-foreground">{tool.description}</p>}</details>
    </li>)}</ul> : <p className="text-sm text-muted-foreground">{tools.length ? "Không có công cụ phù hợp." : "Chưa có dữ liệu công cụ."}</p>}
    <div className="flex flex-wrap items-center justify-between gap-2 text-sm text-muted-foreground">
      <span>{matches.length} công cụ · Trang {current + 1}/{pages}</span>
      <div className="flex gap-2"><Button variant="outline" size="sm" disabled={current === 0} onClick={() => setIndex(current - 1)}>Trước</Button>
      <Button variant="outline" size="sm" disabled={current + 1 === pages} onClick={() => setIndex(current + 1)}>Sau</Button></div>
    </div>
  </div>
}

function deviceName(row: Device) { return row.name ?? ("Robot " + row.id.slice(0, 8)) }

export default function App() {
  const { theme, setTheme } = useTheme()
  const [fleet, setFleet] = useState<Fleet>()
  const [page, setPage] = useState(0)
  const [deviceId, setDeviceId] = useState("")
  const [assetId, setAssetId] = useState("")
  const [port, setPort] = useState("")
  const [usbTools, setUsbTools] = useState<Tool[]>([])
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState("")
  const [connectionError, setConnectionError] = useState(false)
  const [notice, setNotice] = useState("")
  const [renameId, setRenameId] = useState("")
  const [name, setName] = useState("")
  const [packs, setPacks] = useState<Pack[]>()
  const [packId, setPackId] = useState("original")
  useEffect(() => {
    let live = true
    let pending = false
    async function poll() {
      if (pending) return
      pending = true
      try {
        const data = await request<Fleet>("/api/fleet")
        if (live) { setFleet(data); setConnectionError(false) }
      } catch { if (live) setConnectionError(true) }
      finally { pending = false }
    }
    void poll()
    const interval = setInterval(poll, 5000)
    return () => { live = false; clearInterval(interval) }
  }, [])
  useEffect(() => {
    if (page !== 3) return
    let live = true
    request<{ packs: Pack[] }>("/api/interfaces").then(data => { if (live) setPacks(data.packs) })
      .catch(() => { if (live) setError("Không đọc được danh sách gói giao diện.") })
    return () => { live = false }
  }, [page])

  const device = fleet?.devices.find(row => row.id === deviceId)
  const pack = packs?.find(row => row.id === packId)
  const ports = fleet?.ports ?? []
  const selectedPort = ports.some(item => item.device === port) ? port : ports.length === 1 ? ports[0].device : ""
  const assets = fleet?.releases.flatMap(release => release.assets.filter(
    asset => asset.name === device?.variant + ".bin"
  ).map(asset => ({ ...asset, release }))) ?? []
  async function action(fn: () => Promise<void>) {
    setBusy(true); setError(""); setNotice("")
    try { await fn() } catch (e) { setError((e as Error).message) }
    finally { setBusy(false) }
  }

  return <div className="min-h-screen md:grid md:grid-cols-[15rem_minmax(0,1fr)]">
    <aside className="fleet-rail border-b md:border-r md:border-b-0">
      <div className="flex items-center gap-3 px-5 py-6">
        <div className="grid size-9 place-items-center rounded-lg bg-primary text-primary-foreground"><CircuitBoard className="size-5" /></div>
        <div><div className="font-semibold tracking-tight">Xiaozhi Fleet</div><div className="text-xs text-muted-foreground">Quản trị robot</div></div>
      </div>
      <nav aria-label="Quản trị" className="flex flex-wrap gap-1 px-3 pb-4 md:flex-col">
        {pages.map(({ title, icon: Icon }, i) => <Button key={title} type="button" variant="ghost"
          aria-current={page === i ? "page" : undefined} className="fleet-nav justify-start" onClick={() => { setPage(i); setError(""); setNotice("") }}>
          <Icon data-icon="inline-start" />{title}
        </Button>)}
      </nav>
    </aside>

    <main className="min-w-0">
      <header className="flex items-center justify-between gap-3 border-b bg-card px-5 py-3 md:px-8">
        <div className="flex min-w-0 items-center gap-2 text-sm text-muted-foreground"><span className="hidden sm:inline">Quản trị</span><ChevronRight className="hidden size-3.5 sm:block" /> <span className="truncate text-foreground">{pages[page].title}</span></div>
        <div className="flex items-center gap-2"><Badge variant="outline" className="fleet-status" data-connected={fleet?.connected && !connectionError}>
          {connectionError ? "Mất kết nối dashboard" : fleet?.connected ? "Music MCP đã kết nối" : "Music MCP chưa kết nối"}
        </Badge>
        <Button variant="ghost" size="icon" aria-label={theme === "dark" ? "Giao diện sáng" : "Giao diện tối"}
          onClick={() => setTheme(theme === "dark" || (theme === "system" && document.documentElement.classList.contains("dark")) ? "light" : "dark")}>
          {theme === "dark" ? <Sun /> : <Moon />}
        </Button></div>
      </header>
      <div className="mx-auto flex max-w-6xl flex-col gap-6 px-5 py-7 md:px-8">
        <div className="flex flex-wrap items-end justify-between gap-3">
          <div><h1 className="text-3xl font-semibold tracking-tight">{pages[page].title}</h1>
            <p className="mt-2 text-sm text-muted-foreground">{[
              "Theo dõi lần robot kiểm tra OTA và quản lý phiên bản được gán.",
              "Danh mục firmware ổn định và thử nghiệm từ GitHub.",
              "Tìm và duyệt công cụ trên máy chủ hoặc trên robot.",
              "Xem tài nguyên ảnh và chọn gói cập nhật cho TFT 240×240.",
            ][page]}</p>
          </div>
          {page === 1 && <Button disabled={busy} onClick={() => action(async () => setFleet(await request<Fleet>("/api/releases", "POST")))}>
            <RefreshCw data-icon="inline-start" />{busy ? "Đang đồng bộ…" : "Đồng bộ GitHub"}
          </Button>}
        </div>
        {error && <p role="alert" className="rounded-md border bg-card p-3 text-sm text-destructive">{error}</p>}
        {notice && <p role="status" className="rounded-md border bg-card p-3 text-sm">{notice}</p>}
        {!fleet && <p role="status" className="text-sm text-muted-foreground">{connectionError ? "Không đọc được API quản trị. Đang thử lại." : "Đang tải dữ liệu…"}</p>}
        {fleet && page === 0 && <>
          <Card><CardHeader><CardTitle>Robot đã ghi nhận</CardTitle>
            <CardDescription>{fleet.devices.length} thiết bị · làm mới dữ liệu mỗi 5 giây</CardDescription></CardHeader>
            <CardContent>{fleet.devices.length === 0 ? <Empty><EmptyHeader><EmptyMedia variant="icon"><Bot /></EmptyMedia>
              <EmptyTitle>Chưa có robot báo về</EmptyTitle><EmptyDescription>Nhập URL OTA bên dưới vào cấu hình robot và khởi động lại.</EmptyDescription>
            </EmptyHeader></Empty> :
              <Table><TableHeader><TableRow><TableHead>Thiết bị</TableHead><TableHead>Firmware</TableHead><TableHead>Kết nối OTA</TableHead><TableHead>Cập nhật</TableHead></TableRow></TableHeader>
                <TableBody>{fleet.devices.map(row => <TableRow key={row.id}>
                  <TableCell>{renameId === row.id ? <form className="flex items-center gap-2" onSubmit={event => {
                    event.preventDefault()
                    void action(async () => { setFleet(await request<Fleet>("/api/devices/" + row.id + "/name", "POST", { name })); setRenameId("") })
                  }}><Input aria-label="Tên robot mới" value={name} maxLength={60} required onChange={event => setName(event.target.value)} />
                    <Button size="sm" disabled={busy}>Lưu</Button><Button type="button" size="sm" variant="ghost" onClick={() => setRenameId("")}>Hủy</Button></form> :
                    <div className="flex items-center gap-2"><span className="font-medium">{deviceName(row)}</span>
                      <Button size="sm" variant="ghost" onClick={() => { setRenameId(row.id); setName(row.name ?? "") }}>Đổi tên</Button></div>}
                    <div className="text-xs text-muted-foreground" title={row.id}>{row.variant} · {row.id.slice(0, 8)}</div></TableCell>
                  <TableCell>{row.version || "—"}{row.target && <div className="text-xs text-muted-foreground">Bản gán {row.target}</div>}</TableCell>
                  <TableCell><Badge variant={row.ota_seen ? "secondary" : "outline"}>{row.ota_seen ? "Đã báo về Fleet" : "Chưa kết nối Fleet"}</Badge>
                    <div className="mt-1 text-xs text-muted-foreground">{row.ota_seen ? new Date(row.ota_seen * 1000).toLocaleString("vi-VN") : "USB không xác nhận OTA"}</div></TableCell>
                  <TableCell><Badge variant={row.state === "installed" ? "secondary" : "outline"}>{status[row.state]}</Badge></TableCell>
                </TableRow>)}</TableBody>
              </Table>}</CardContent>
          </Card>
          <div className="grid gap-4 lg:grid-cols-2">
            <Card><CardHeader><CardTitle>Gán bản cập nhật</CardTitle><CardDescription>Robot nhận bản được gán khi kiểm tra OTA lần tới.</CardDescription></CardHeader>
              <CardContent><form onSubmit={event => {
                event.preventDefault()
                void action(async () => { setFleet(await request<Fleet>("/api/assignments", "POST", { device: deviceId, asset: Number(assetId) })); setNotice("Đã gán firmware cho robot.") })
              }}><FieldGroup>
                <Field><FieldLabel htmlFor="device">Robot</FieldLabel><select className={selectClass} id="device" value={deviceId} onChange={e => { setDeviceId(e.target.value); setAssetId("") }}>
                  <option value="">Chọn robot</option>{fleet.devices.map(row => <option key={row.id} value={row.id}>{deviceName(row)}</option>)}
                </select></Field>
                <Field><FieldLabel htmlFor="asset">Firmware tương thích</FieldLabel><select className={selectClass} id="asset" value={assetId} onChange={e => setAssetId(e.target.value)}>
                  <option value="">Chọn bản phát hành</option>{assets.map(asset => <option key={asset.id} value={asset.id}>{asset.release.tag} · {asset.release.preview ? "Thử nghiệm" : "Ổn định"}</option>)}
                </select></Field>
                {device && !assets.length && <p className="text-sm text-muted-foreground">Đồng bộ GitHub ở mục Bản phát hành để tải danh mục file.</p>}
                <div className="flex flex-wrap gap-2">
                  <Button type="submit" disabled={busy || !deviceId || !assetId}>{busy ? "Đang xử lý…" : "Gán firmware"}</Button>
                  <Button type="button" variant="outline" disabled={busy || !device?.target} onClick={() => action(async () => {
                    setFleet(await request<Fleet>("/api/assignments/" + encodeURIComponent(deviceId), "DELETE")); setNotice("Đã bỏ gán firmware.")
                  })}>Bỏ gán</Button>
                </div>
              </FieldGroup></form></CardContent>
            </Card>
            <Card><CardHeader><CardTitle>Kết nối robot</CardTitle><CardDescription>Dùng URL OTA trên mạng mà robot truy cập được. Giữ URL này riêng tư.</CardDescription></CardHeader>
              <CardContent className="flex flex-col gap-3"><Field><FieldLabel htmlFor="ota-url">URL OTA</FieldLabel><div className="flex gap-2">
                <Input className="min-w-0" id="ota-url" readOnly value={fleet.ota_url} onFocus={event => event.target.select()} />
                <Button variant="outline" size="icon" aria-label="Sao chép URL OTA" onClick={() => action(async () => {
                  await navigator.clipboard.writeText(fleet.ota_url)
                  setNotice("Đã sao chép URL OTA.")
                })}><Copy /></Button>
              </div></Field>
                <p className="text-sm text-muted-foreground">LAN dùng IP của máy chạy server. Khi đổi Wi‑Fi, kiểm tra và cập nhật URL trên robot nếu IP thay đổi. VPS nên dùng tên miền HTTPS.</p>
              </CardContent></Card>
          </div>
        </>}
        {fleet && page === 1 && <Card><CardHeader><CardTitle>GitHub Releases</CardTitle><CardDescription>{fleet.repository}</CardDescription></CardHeader><CardContent>
          {!fleet.releases.length ? <Empty><EmptyHeader><EmptyMedia variant="icon"><Boxes /></EmptyMedia><EmptyTitle>Chưa có dữ liệu phát hành</EmptyTitle>
            <EmptyDescription>Bấm Đồng bộ GitHub để xem các bản và file đính kèm.</EmptyDescription></EmptyHeader></Empty> :
            <Table><TableHeader><TableRow><TableHead>Phiên bản</TableHead><TableHead>Kênh</TableHead><TableHead>File phát hành</TableHead></TableRow></TableHeader>
              <TableBody>{fleet.releases.map(release => <TableRow key={release.tag}>
                <TableCell><a className="font-medium underline-offset-4 hover:underline" href={release.url} target="_blank" rel="noreferrer">{release.tag}</a></TableCell>
                <TableCell><Badge variant={release.preview ? "outline" : "secondary"}>{release.preview ? "Thử nghiệm" : "Ổn định"}</Badge></TableCell>
                <TableCell>{release.assets.length ? release.assets.map(asset => <div key={asset.id} className="py-1"><code>{asset.name}</code> <span className="text-muted-foreground">· {(asset.size / 1048576).toFixed(2)} MiB</span></div>) : "Chưa có file"}</TableCell>
              </TableRow>)}</TableBody>
            </Table>}
        </CardContent></Card>}
        {fleet && page === 2 && <div className="grid items-start gap-4 lg:grid-cols-2">
          <Card><CardHeader><CardTitle>Music MCP</CardTitle>
            <CardDescription>{fleet.music.title || "Chưa có bài nhạc"} · {fleet.music.status ?? "idle"}</CardDescription></CardHeader>
            <CardContent><ToolList tools={fleet.tools} /></CardContent></Card>
          <Card className="max-lg:order-first"><CardHeader><CardTitle>Tool trên robot</CardTitle><CardDescription>Đọc danh sách tool và ghi nhận phiên bản robot qua USB Serial.</CardDescription></CardHeader>
            <CardContent><form className="mb-4" onSubmit={event => { event.preventDefault(); setUsbTools([]); void action(async () => setUsbTools((await request<{tools: Tool[]}>("/api/usb/tools", "POST", {port: selectedPort})).tools)) }}>
              <FieldGroup><Field><FieldLabel htmlFor="usb-port">Cổng Serial trên server</FieldLabel>
                <select id="usb-port" className={selectClass} value={selectedPort} onChange={e => setPort(e.target.value)}>
                  <option value="">{ports.length ? "Chọn cổng Serial" : "Chưa thấy cổng Serial"}</option>
                  {ports.map(item => <option key={item.device} value={item.device}>{item.device} · {item.description}</option>)}
                </select>
                {ports.length === 0 && <p className="text-sm text-muted-foreground">Cắm USB vào máy chạy Music MCP. Danh sách tự làm mới mỗi 5 giây.</p>}
              </Field>
              <Button type="submit" disabled={busy || !selectedPort}><Cable data-icon="inline-start" />{busy ? "Đang đọc…" : "Đọc công cụ"}</Button></FieldGroup>
            </form><ToolList tools={usbTools} /></CardContent></Card>
        </div>}
        {fleet && page === 3 && <div className="grid items-start gap-4 lg:grid-cols-[minmax(0,1fr)_19rem]">
          <Card><CardHeader><CardTitle>Ảnh trong gói</CardTitle><CardDescription>Ảnh hiển thị 1 pixel CSS trên mỗi pixel của file. RoboEyes và đồng hồ chờ được vẽ bằng firmware.</CardDescription></CardHeader>
            <CardContent>{pack ? <ul className="grid grid-cols-[repeat(auto-fit,minmax(7rem,1fr))] gap-3">
              {pack.previews.map(preview => <li className="flex min-w-0 flex-col items-center gap-2 overflow-x-auto rounded-lg border bg-muted p-2 text-center text-xs" key={preview.file}>
                <img className="max-w-none object-contain" src={`/api/interfaces/${pack.id}/${preview.file}`} alt={preview.name} width={preview.width} height={preview.height} loading="lazy" />
                <span>{preview.name} · {preview.width}×{preview.height}</span>
              </li>)}</ul> : <p className="text-sm text-muted-foreground">{packs ? "Chưa cài gói giao diện trên server." : "Đang đọc gói…"}</p>}</CardContent>
          </Card>
          <Card className="max-lg:order-first"><CardHeader><CardTitle>Cập nhật tài nguyên</CardTitle><CardDescription>USB gửi lệnh. Robot tải gói qua Wi‑Fi sau khi khởi động lại.</CardDescription></CardHeader>
            <CardContent><form onSubmit={event => { event.preventDefault(); void action(async () => {
              await request("/api/interfaces/apply", "POST", {device: deviceId, port: selectedPort, pack: packId})
              setNotice("Đã gửi lệnh và yêu cầu khởi động lại. Theo dõi tiến độ trên màn hình robot.")
            }) }}><FieldGroup>
              <Field><FieldLabel htmlFor="interface-pack">Gói giao diện</FieldLabel><select id="interface-pack" className={selectClass} value={packId} onChange={e => setPackId(e.target.value)}>
                {packs?.map(item => <option key={item.id} value={item.id}>{item.title}</option>)}
              </select></Field>
              <Field><FieldLabel htmlFor="interface-device">Robot TFT 240×240</FieldLabel><select id="interface-device" className={selectClass} value={deviceId} onChange={e => setDeviceId(e.target.value)}>
                <option value="">Chọn robot</option>{fleet.devices.filter(row => row.variant === "bread-compact-wifi-lcd-240x240").map(row => <option key={row.id} value={row.id}>{deviceName(row)}</option>)}
              </select></Field>
              <Field><FieldLabel htmlFor="interface-port">Cổng Serial</FieldLabel><select id="interface-port" className={selectClass} value={selectedPort} onChange={e => setPort(e.target.value)}>
                <option value="">Chọn cổng</option>{ports.map(item => <option key={item.device}>{item.device}</option>)}
              </select></Field>
              <Button type="submit" disabled={busy || !pack || !selectedPort || device?.variant !== "bread-compact-wifi-lcd-240x240"}>Cập nhật và khởi động lại</Button>
            </FieldGroup></form></CardContent>
          </Card>
        </div>}
      </div>
    </main>
  </div>
}
