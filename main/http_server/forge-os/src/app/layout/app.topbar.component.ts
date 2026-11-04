import { Component, ElementRef, Input, ViewChild } from '@angular/core';
import { ActivatedRoute, NavigationEnd, Router } from '@angular/router';
import { ToastrService } from 'ngx-toastr';
import { filter, map, startWith, type Observable } from 'rxjs';

import { LayoutService } from './service/app.layout.service';
import { SystemService } from '../services/system.service';
import type { ISystemInfo } from '../../models/ISystemInfo';
import { heatColor, hottestTemp } from '../utils/heat';
import { HashSuffixPipe } from '../pipes/hash-suffix.pipe';
import { matchPerformanceMode } from '../utils/performance-modes';

@Component({
    selector: 'app-topbar',
    templateUrl: './app.topbar.component.html',
    standalone: false
})
export class AppTopBarComponent {

    public info$: Observable<ISystemInfo>;
    public title$: Observable<string>;

    @Input() isAPMode: boolean = false;

    @ViewChild('menubutton') menuButton!: ElementRef;

    constructor(
        public layoutService: LayoutService,
        private systemService: SystemService,
        private toastr: ToastrService,
        router: Router,
        route: ActivatedRoute,
    ) {
        this.info$ = this.systemService.info$;

        // Title comes from the deepest active route's `data.title`.
        this.title$ = router.events.pipe(
            filter((event) => event instanceof NavigationEnd),
            startWith(null),
            map(() => {
                let current = route.snapshot;
                while (current.firstChild) current = current.firstChild;
                return current.data['title'] ?? 'ForgeOS';
            }),
        );
    }

    public restart() {
        this.systemService.restart().subscribe(() => {});
        this.toastr.success('BitForge is restarting.', 'Restarting');
    }

    public hottestChip(info: ISystemInfo): string {
        const temps = [info.chiptemp1, info.chiptemp2, info.temp].filter((t): t is number => !!t && t > 0);
        return temps.length ? Math.max(...temps).toFixed(0) : '--';
    }

    public hashrate(info: ISystemInfo): string {
        return HashSuffixPipe.transform(info.hashRate * 1000000000);
    }

    public heat(info: ISystemInfo): string {
        return heatColor(hottestTemp([info.chiptemp1, info.chiptemp2, info.temp], info.vrTemp));
    }

    public getPerformanceMode(info: ISystemInfo): { label: string, class: string } {
        const mode = matchPerformanceMode(info.frequency, info.coreVoltage);
        if (!mode) return { label: 'Custom', class: '' };
        return { label: mode.label, class: { eco: 'is-ok', medium: 'is-warn', power: 'is-bad' }[mode.id] };
    }

}
