import { Pipe, PipeTransform } from '@angular/core';

@Pipe({
    name: 'sats',
    standalone: false
})
export class SatsPipe implements PipeTransform {
  transform(value: number | undefined | null): string {
    if (!value) return '0 BTC';
    return `${(value / 100_000_000).toFixed(8)} BTC`;
  }
}
