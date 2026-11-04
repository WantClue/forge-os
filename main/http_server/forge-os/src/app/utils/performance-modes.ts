// Presets offered on the Settings page. The top bar uses the same list to
// name the mode the miner is running in, so the two always agree.
export type PerformanceModeId = 'eco' | 'medium' | 'power';

export interface PerformanceMode {
  id: PerformanceModeId;
  label: string;
  frequency: number;   // MHz
  coreVoltage: number; // mV
  description: string;
}

export const PERFORMANCE_MODES: PerformanceMode[] = [
  { id: 'eco', label: 'Eco', frequency: 430, coreVoltage: 1000, description: 'Lowest power and heat' },
  { id: 'medium', label: 'Medium', frequency: 525, coreVoltage: 1150, description: 'Factory default' },
  { id: 'power', label: 'Power', frequency: 610, coreVoltage: 1200, description: 'Most hashrate, runs hottest' },
];

export function matchPerformanceMode(frequency: number, coreVoltage: number): PerformanceMode | undefined {
  return PERFORMANCE_MODES.find((m) => m.frequency === frequency && m.coreVoltage === coreVoltage);
}
