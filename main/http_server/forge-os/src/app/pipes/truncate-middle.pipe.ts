import { Pipe, PipeTransform } from '@angular/core';

@Pipe({
  name: 'truncateMiddle'
})
export class TruncateMiddlePipe implements PipeTransform {
  transform(value: string, startChars: number = 10, endChars: number = 8): string {
    if (!value || value.length <= startChars + endChars) {
      return value;
    }

    const start = value.substring(0, startChars);
    const end = value.substring(value.length - endChars);

    return `${start}...${end}`;
  }
}
