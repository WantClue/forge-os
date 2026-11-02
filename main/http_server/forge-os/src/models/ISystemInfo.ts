import { eASICModel } from './enum/eASICModel';

interface ISharesRejectedStat {
    message: string;
    count: number;
}

export interface ICoinbaseOutput {
    address: string;
    value: number;
}

export interface ICoinbase {
    blockHeight: number;
    scriptsig: string;
    valueTotalSatoshis: number;
    outputs: ICoinbaseOutput[];
}

interface IStratumPoolInfo {
    connected: boolean;
    validNotify: boolean;
    difficulty: number;
    accepted: number;
    rejected: number;
    coinbase?: ICoinbase;
}

export type BlockStatus = 'none' | 'pending' | 'confirmed' | 'orphaned';

export interface ISystemInfo {

    power: number,
    voltage: number,
    current: number,
    temp: number,
    vrTemp: number,
    maxPower: number,
    nominalVoltage: number,
    hashRate: number,
    bestDiff: number,
    bestSessionDiff: string,
    stratumDiff: number,
    blocksFound: number,
    lastBlockHash: string,
    lastBlockStatus: BlockStatus,
    lastBlockTime: number,
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
    poolMode: number,
    poolBalance: number,
    pools: IStratumPoolInfo[],
    coinbase?: ICoinbase,
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
    stratumTLS: number,
    fallbackStratumTLS: number,
    stratumCert: string,
    fallbackStratumCert: string,
    stratumProtocol: number,
    fallbackStratumProtocol: number,
    stratumV2ChannelType: number,
    fallbackStratumV2ChannelType: number,
    stratumV2AuthorityPubkey: string,
    fallbackStratumV2AuthorityPubkey: string,
    stratumV2RequireAuth: boolean,
    fallbackStratumV2RequireAuth: boolean,
    poolsUnavailable: boolean,
    frequency: number,
    version: string,
    idfVersion: string,
    autofanspeed: number,
    fanSpeed: number,
    manualFanSpeed: number,
    fanrpm: number,
    coreVoltageActual: number,

    chiptemp1?: number,
    chiptemp2?: number,
    overheat_mode: number,
    power_fault?: string
    overclockEnabled?: number
}
