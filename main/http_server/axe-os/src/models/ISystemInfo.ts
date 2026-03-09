import { eASICModel } from './enum/eASICModel';

interface ISharesRejectedStat {
    message: string;
    count: number;
}

interface IHashrateMonitorAsic {
    total: number;
    domains: number[];
    errorCount: number;
}

export interface ISystemInfo {

    power: number,
    voltage: number,
    current: number,
    temp: number,
    vrTemp: number,
    maxPower: number,
    nominalVoltage: number,
    hashRate: number,
    expectedHashrate?: number,
    errorPercentage?: number,
    bestDiff: number,
    bestSessionDiff: string,
    freeHeap: number,
    freeHeapInternal: number,
    freeHeapSpiram: number,
    isPSRAMAvailable: number,
    coreVoltage: number,
    hostname: string,
    macAddr: string,
    ssid: string,
    wifiStatus: string,
    apEnabled: number,
    sharesAccepted: number,
    sharesRejected: number,
    sharesRejectedReasons: ISharesRejectedStat[];
    uptimeSeconds: number,
    asicCount: number,
    smallCoreCount: number,
    ASICModel: eASICModel,
    stratumURL: string,
    stratumPort: number,
    fallbackStratumURL: string,
    fallbackStratumPort: number,
    isUsingFallbackStratum: boolean,
    stratumUser: string,
    fallbackStratumUser: string,
    frequency: number,
    version: string,
    idfVersion: string,
    boardVersion: string,
    autofanspeed: number,
    fanspeed: number,
    fanrpm: number,
    coreVoltageActual: number,

    chiptemp1?: number,
    chiptemp2?: number,
    overheat_mode: number,
    power_fault?: string
    overclockEnabled?: number,

    blockHeight?: number,
    scriptsig?: string,
    networkDifficulty?: number,

    blockFound?: boolean,
    fanTargetTemp?: number,
    fanMinSpeed?: number,

    coinbaseOutputs?: { valueSatoshis: number, address: string, isUserOutput: boolean }[],
    coinbaseValueTotal?: number,

    hashrateMonitor?: {
        asics: IHashrateMonitorAsic[];
    },
}
