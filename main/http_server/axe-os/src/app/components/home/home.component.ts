import { Component } from "@angular/core"
import { interval, map, type Observable, shareReplay, startWith, switchMap, tap } from "rxjs"
import { HashSuffixPipe } from "src/app/pipes/hash-suffix.pipe"
import { TruncateMiddlePipe } from "src/app/pipes/truncate-middle.pipe"
import { SystemService } from "src/app/services/system.service"
import type { ISystemInfo } from "src/models/ISystemInfo"
import { ToastrService } from "ngx-toastr"

@Component({
  selector: "app-home",
  templateUrl: "./home.component.html",
  styleUrls: ["./home.component.scss"],
})
export class HomeComponent {
  public info$!: Observable<ISystemInfo>
  public quickLink$!: Observable<string | undefined>
  public fallbackQuickLink$!: Observable<string | undefined>
  public expectedHashRate$!: Observable<number | undefined>

  public chartOptions: any
  public dataLabel: number[] = []
  public hashrateData: number[] = []
  public powerData: number[] = []
  public chartY1Data: number[] = []
  public chartY2Data: number[] = []
  public chartData?: any

  public maxPower = 0
  public nominalVoltage = 0
  public maxTemp = 75
  public maxFrequency = 800

  public chartY1Key = localStorage.getItem('chartY1') ?? 'hashrate'
  public chartY2Key = localStorage.getItem('chartY2') ?? 'temperature'

  public readonly chartMetrics = [
    { label: 'Hashrate',    key: 'hashrate' },
    { label: 'Temperature', key: 'temperature' },
    { label: 'Power',       key: 'power' },
    { label: 'Efficiency',  key: 'efficiency' },
    { label: 'Fan Speed',   key: 'fanspeed' },
    { label: 'Error %',     key: 'errorPercentage' },
    { label: 'None',        key: 'none' },
  ]

  constructor(
    private systemService: SystemService,
    private toastr: ToastrService,
  ) {
    this.initializeChart()
  }

  private updateChartColors() {
    // Modern color scheme for dark theme
    const textColor = '#f1f5f9'
    const textColorSecondary = '#94a3b8'
    const surfaceBorder = '#334155'
    const primaryColor = '#6366f1'
    const temperatureColor = '#ea580c'

    // Update chart colors with modern gradients
    if (this.chartData && this.chartData.datasets) {
      // Hash rate dataset with gradient
      this.chartData.datasets[0].backgroundColor = 'rgba(99, 102, 241, 0.1)'
      this.chartData.datasets[0].borderColor = primaryColor
      this.chartData.datasets[0].fill = 'start'

      // Temperature dataset with gradient
      if (this.chartData.datasets[1]) {
        this.chartData.datasets[1].backgroundColor = 'rgba(234, 88, 12, 0.1)'
        this.chartData.datasets[1].borderColor = temperatureColor
        this.chartData.datasets[1].fill = 'start'
      }
    }

    // Update chart options with modern styling
    if (this.chartOptions) {
      this.chartOptions.plugins.legend.labels.color = textColor
      this.chartOptions.plugins.legend.labels.font = {
        size: 14,
        weight: 500
      }

      this.chartOptions.scales.x.ticks.color = textColorSecondary
      this.chartOptions.scales.x.grid.color = surfaceBorder
      this.chartOptions.scales.x.grid.drawBorder = false

      this.chartOptions.scales.y.ticks.color = textColorSecondary
      this.chartOptions.scales.y.grid.color = surfaceBorder
      this.chartOptions.scales.y.grid.drawBorder = false

      this.chartOptions.scales.y2.ticks.color = textColorSecondary
      this.chartOptions.scales.y2.grid.drawOnChartArea = false
      this.chartOptions.scales.y2.grid.color = surfaceBorder
    }

    // Force chart update
    this.chartData = { ...this.chartData }
  }

  private initializeChart() {
    // Modern color scheme for dark theme
    const textColor = '#f1f5f9'
    const textColorSecondary = '#94a3b8'
    const surfaceBorder = '#334155'
    const primaryColor = '#6366f1'
    const temperatureColor = '#ea580c'

    this.chartData = {
      labels: [],
      datasets: [
        {
          type: "line",
          label: this.getAxisLabel(this.chartY1Key),
          data: [],
          backgroundColor: 'rgba(99, 102, 241, 0.1)',
          borderColor: primaryColor,
          tension: 0.4,
          pointRadius: 1,
          pointHoverRadius: 6,
          borderWidth: 3,
          yAxisID: "y",
          fill: 'start',
        },
        {
          type: "line",
          label: this.getAxisLabel(this.chartY2Key),
          data: [],
          backgroundColor: 'rgba(234, 88, 12, 0.1)',
          borderColor: temperatureColor,
          tension: 0.4,
          pointRadius: 1,
          pointHoverRadius: 6,
          borderWidth: 3,
          yAxisID: "y2",
          fill: 'start',
        },
      ],
    }

    this.chartOptions = {
      animation: false,
      maintainAspectRatio: false,
      interaction: {
        intersect: false,
        mode: 'index'
      },
      plugins: {
        legend: {
          labels: {
            color: textColor,
            font: {
              size: 14,
              weight: 500
            },
            usePointStyle: true,
            padding: 20
          },
        },
        tooltip: {
          backgroundColor: 'rgba(30, 41, 59, 0.95)',
          titleColor: textColor,
          bodyColor: textColor,
          borderColor: 'rgba(99, 102, 241, 0.3)',
          borderWidth: 1,
          cornerRadius: 8,
          displayColors: true,
          callbacks: {
            label: (tooltipItem: any) => {
              let label = tooltipItem.dataset.label || ""
              if (label) {
                label += ": "
              }
              if (tooltipItem.dataset.label.includes("Temperature")) {
                label += tooltipItem.raw + "°C"
              } else {
                label += HashSuffixPipe.transform(tooltipItem.raw)
              }
              return label
            },
          },
        },
      },
      scales: {
        x: {
          type: "time",
          time: {
            unit: "hour",
          },
          ticks: {
            color: textColorSecondary,
            font: {
              size: 12
            }
          },
          grid: {
            color: surfaceBorder,
            drawBorder: false,
            display: true,
          },
        },
        y: {
          ticks: {
            color: textColorSecondary,
            font: {
              size: 12
            },
            callback: (value: number) => this.formatAxisValue(this.chartY1Key, value),
          },
          grid: {
            color: surfaceBorder,
            drawBorder: false,
          },
          beginAtZero: true,
        },
        y2: {
          type: "linear",
          display: true,
          position: "right",
          ticks: {
            color: textColorSecondary,
            font: {
              size: 12
            },
            callback: (value: number) => this.formatAxisValue(this.chartY2Key, value),
          },
          grid: {
            drawOnChartArea: false,
            color: surfaceBorder,
          },
          suggestedMax: this.maxTemp,
        },
      },
    }

    this.info$ = interval(5000).pipe(
      startWith(() => this.systemService.getInfo()),
      switchMap(() => {
        return this.systemService.getInfo()
      }),
      tap((info) => {
        // Only collect and update chart data if there's no power fault
        if (!info.power_fault) {
          this.hashrateData.push(info.hashRate * 1000000000)
          this.powerData.push(info.power)
          this.chartY1Data.push(this.getDataPoint(this.chartY1Key, info))
          this.chartY2Data.push(this.getDataPoint(this.chartY2Key, info))

          this.dataLabel.push(new Date().getTime())

          if (this.hashrateData.length >= 720) {
            this.hashrateData.shift()
            this.powerData.shift()
            this.chartY1Data.shift()
            this.chartY2Data.shift()
            this.dataLabel.shift()
          }

          const y1Max = this.getSuggestedMax(this.chartY1Key, info)
          const y2Max = this.getSuggestedMax(this.chartY2Key, info)
          if (y1Max !== undefined) this.chartOptions.scales.y.suggestedMax = y1Max
          if (y2Max !== undefined) this.chartOptions.scales.y2.suggestedMax = y2Max

          this.chartData.labels = this.dataLabel
          this.chartData.datasets[0].data = this.chartY1Data
          this.chartData.datasets[1].data = this.chartY2Data

          // Force chart update with reference change
          this.chartData = {
            ...this.chartData,
          }
        }

        this.maxPower = Math.max(info.maxPower, info.power)
        this.nominalVoltage = info.nominalVoltage
        // Use the maximum of both ASIC chip temperatures for the max temperature scale
        const maxAsicTemp = Math.max(info.chiptemp1 || 0, info.chiptemp2 || 0, info.temp || 0)
        this.maxTemp = Math.max(75, maxAsicTemp)
        this.maxFrequency = Math.max(800, info.frequency)
      }),
      map((info) => {
        info.power = Number.parseFloat(info.power.toFixed(1))
        info.voltage = Number.parseFloat((info.voltage / 1000).toFixed(1))
        info.current = Number.parseFloat((info.current / 1000).toFixed(1))
        info.coreVoltageActual = Number.parseFloat((info.coreVoltageActual / 1000).toFixed(2))
        info.coreVoltage = Number.parseFloat((info.coreVoltage / 1000).toFixed(2))
        info.temp = Number.parseFloat(info.temp.toFixed(1))
        if (info.chiptemp1) info.chiptemp1 = Number.parseFloat(info.chiptemp1.toFixed(1))
        if (info.chiptemp2) info.chiptemp2 = Number.parseFloat(info.chiptemp2.toFixed(1))

        return info
      }),
      shareReplay({ refCount: true, bufferSize: 1 }),
    )

    this.expectedHashRate$ = this.info$.pipe(
      map((info) => {
        return Math.floor(info.frequency * ((info.smallCoreCount * info.asicCount) / 1000))
      }),
    )

    this.quickLink$ = this.info$.pipe(map((info) => this.getQuickLink(info.stratumURL, info.stratumUser)))

    this.fallbackQuickLink$ = this.info$.pipe(
      map((info) => this.getQuickLink(info.fallbackStratumURL, info.fallbackStratumUser)),
    )
  }

  public calculateAverage(data: number[]): number {
    if (data.length === 0) return 0
    const sum = data.reduce((sum, value) => sum + value, 0)
    return sum / data.length
  }

  private getQuickLink(stratumURL: string, stratumUser: string): string | undefined {
    const address = stratumUser.split(".")[0]

    if (stratumURL.includes("public-pool.io")) {
      return `https://web.public-pool.io/#/app/${address}`
    } else if (stratumURL.includes("ocean.xyz")) {
      return `https://ocean.xyz/stats/${address}`
    } else if (stratumURL.includes("solo.d-central.tech")) {
      return `https://solo.d-central.tech/#/app/${address}`
    } else if (/^eusolo[46]?.ckpool.org/.test(stratumURL)) {
      return `https://eusolostats.ckpool.org/users/${address}`
    } else if (/^solo[46]?.ckpool.org/.test(stratumURL)) {
      return `https://solostats.ckpool.org/users/${address}`
    } else if (stratumURL.includes("pool.noderunners.network")) {
      return `https://noderunners.network/en/pool/user/${address}`
    } else if (stratumURL.includes("satoshiradio.nl")) {
      return `https://pool.satoshiradio.nl/user/${address}`
    } else if (stratumURL.includes("solohash.co.uk")) {
      return `https://solohash.co.uk/user/${address}`
    }
    return stratumURL.startsWith("http") ? stratumURL : `http://${stratumURL}`
  }

  public getAsicsAmount(info: ISystemInfo): number {
    return info.hashrateMonitor?.asics?.length ?? 0
  }

  public getAsicDomainsAmount(info: ISystemInfo): number {
    return info.hashrateMonitor?.asics[0]?.domains?.length ?? 0
  }

  public getHeatmapColor(info: ISystemInfo, domainHashrate: number): string {
    const expectedPerDomain = (info.expectedHashrate ?? 0) / this.getAsicDomainsAmount(info) / this.getAsicsAmount(info)
    if (expectedPerDomain <= 0) return 'rgb(30, 41, 59)'

    const ratio = Math.max(0, Math.min(2, domainHashrate / expectedPerDomain))
    const deviation = Math.abs(ratio - 1)
    const t = 1 - Math.pow(1 - deviation, 3)
    const target = ratio > 1 ? 255 : 0

    const r = 99, g = 102, b = 241 // #6366f1
    const finalR = ((r * (1 - t) + target * t) | 0)
    const finalG = ((g * (1 - t) + target * t) | 0)
    const finalB = ((b * (1 - t) + target * t) | 0)

    return `rgb(${finalR}, ${finalG}, ${finalB})`
  }

  public getAxisLabel(key: string): string {
    return this.chartMetrics.find(m => m.key === key)?.label ?? key
  }

  public formatAxisValue(key: string, value: number): string {
    switch (key) {
      case 'hashrate':         return HashSuffixPipe.transform(value)
      case 'temperature':      return value + '°C'
      case 'power':            return value.toFixed(1) + 'W'
      case 'efficiency':       return value.toFixed(1) + ' J/TH'
      case 'fanspeed':         return value + '%'
      case 'errorPercentage':  return value.toFixed(2) + '%'
      default:                 return String(value)
    }
  }

  private getSuggestedMax(key: string, info: ISystemInfo): number | undefined {
    switch (key) {
      case 'temperature':     return this.maxTemp
      case 'power':           return Math.max(this.maxPower, 1)
      case 'fanspeed':        return 100
      case 'errorPercentage': return Math.max(10, (info.errorPercentage ?? 0) * 1.5)
      default:                return undefined
    }
  }

  private getDataPoint(key: string, info: ISystemInfo): number {
    switch (key) {
      case 'hashrate':        return info.hashRate * 1e9
      case 'temperature':
        if (info.chiptemp1 && info.chiptemp2) return (info.chiptemp1 + info.chiptemp2) / 2
        return info.chiptemp1 || info.chiptemp2 || info.temp || 0
      case 'power':           return info.power
      case 'efficiency':      return info.hashRate > 0 ? info.power / (info.hashRate / 1000) : 0
      case 'fanspeed':        return info.fanspeed
      case 'errorPercentage': return info.errorPercentage ?? 0
      default:                return 0
    }
  }

  public onAxisChange(): void {
    localStorage.setItem('chartY1', this.chartY1Key)
    localStorage.setItem('chartY2', this.chartY2Key)

    this.chartY1Data.length = 0
    this.chartY2Data.length = 0

    this.chartData.datasets[0].label = this.getAxisLabel(this.chartY1Key)
    this.chartData.datasets[1].label = this.getAxisLabel(this.chartY2Key)

    this.chartOptions.scales.y.display = this.chartY1Key !== 'none'
    this.chartOptions.scales.y2.display = this.chartY2Key !== 'none'
    this.chartOptions.scales.y.suggestedMax = undefined
    this.chartOptions.scales.y2.suggestedMax = undefined

    this.chartData = { ...this.chartData }
    this.chartOptions = { ...this.chartOptions }
  }

  public switchPool(useFallback: boolean): void {
    this.systemService.updateSystem('', { useFallbackStratum: useFallback ? 1 : 0 }).subscribe({
      next: () => this.toastr.success(`Switched to ${useFallback ? 'fallback' : 'primary'} pool`, 'Pool Switch'),
      error: () => this.toastr.error('Failed to switch pool', 'Error'),
    })
  }

  public getWinProbability(bestDiff: number, networkDiff: number | undefined): string {
    if (!networkDiff || networkDiff <= 0 || !bestDiff || bestDiff <= 0) return ''
    const prob = (bestDiff / networkDiff) * 100
    if (prob < 0.01) return `< 0.01%`
    if (prob >= 100) return `100%`
    return `${prob.toFixed(prob < 1 ? 2 : 1)}%`
  }

  public getWinProbabilityFull(bestDiff: number, networkDiff: number | undefined): string {
    if (!networkDiff || networkDiff <= 0 || !bestDiff || bestDiff <= 0) return ''
    const prob = (bestDiff / networkDiff) * 100
    return `${prob.toPrecision(6)}%`
  }

  public calculateEfficiencyAverage(hashrateData: number[], powerData: number[]): number {
    if (hashrateData.length === 0 || powerData.length === 0) return 0

    // Calculate efficiency for each data point and average them
    const efficiencies = hashrateData.map((hashrate, index) => {
      const power = powerData[index] || 0
      return power / (hashrate / 1000000000000) // Convert to J/TH
    })

    return this.calculateAverage(efficiencies)
  }

}
