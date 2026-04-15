import { Component } from "@angular/core"
import { interval, map, type Observable, shareReplay, startWith, switchMap, tap } from "rxjs"
import { HashSuffixPipe } from "src/app/pipes/hash-suffix.pipe"
import { TruncateMiddlePipe } from "src/app/pipes/truncate-middle.pipe"
import { QuicklinkService } from "src/app/services/quicklink.service"
import { SystemService } from "src/app/services/system.service"
import type { ISystemInfo } from "src/models/ISystemInfo"

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
  public temperatureData: number[] = []
  public powerData: number[] = []
  public chartData?: any

  public maxPower = 0
  public nominalVoltage = 0
  public maxTemp = 75
  public maxFrequency = 800

  constructor(
    private systemService: SystemService,
    private quicklinkService: QuicklinkService,
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
          label: "Hash Rate (TH/s)",
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
          label: "Temperature (°C)",
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
            callback: (value: number) => HashSuffixPipe.transform(value),
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
            callback: (value: number) => value + "°C",
          },
          grid: {
            drawOnChartArea: false,
            color: surfaceBorder,
          },
          suggestedMax: 80,
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
          // Use the average of both ASIC chip temperatures for the chart
          const avgTemp =
            info.chiptemp1 && info.chiptemp2
              ? (info.chiptemp1 + info.chiptemp2) / 2
              : info.chiptemp1 || info.chiptemp2 || info.temp
          this.temperatureData.push(avgTemp)
          this.powerData.push(info.power)

          this.dataLabel.push(new Date().getTime())

          if (this.hashrateData.length >= 720) {
            this.hashrateData.shift()
            this.temperatureData.shift()
            this.powerData.shift()
            this.dataLabel.shift()
          }

          this.chartData.labels = this.dataLabel
          this.chartData.datasets[0].data = this.hashrateData
          this.chartData.datasets[1].data = this.temperatureData

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

    this.quickLink$ = this.info$.pipe(map((info) => this.quicklinkService.getQuickLink(info.stratumURL, info.stratumUser)))

    this.fallbackQuickLink$ = this.info$.pipe(
      map((info) => this.quicklinkService.getQuickLink(info.fallbackStratumURL, info.fallbackStratumUser)),
    )
  }

  public calculateAverage(data: number[]): number {
    if (data.length === 0) return 0
    const sum = data.reduce((sum, value) => sum + value, 0)
    return sum / data.length
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
