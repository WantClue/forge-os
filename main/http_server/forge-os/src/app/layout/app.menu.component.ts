import { Component, OnInit } from '@angular/core';
import { Router } from '@angular/router';
import { ToastrService } from 'ngx-toastr';

import { SystemService } from '../services/system.service';
import { LayoutService } from './service/app.layout.service';

@Component({
    selector: 'app-menu',
    templateUrl: './app.menu.component.html'
})
export class AppMenuComponent implements OnInit {

    model: any[] = [];

    constructor(public layoutService: LayoutService,
        private systemService: SystemService,
        private toastr: ToastrService,
        private router: Router
    ) { }

    ngOnInit() {
        this.model = [
            {
                label: 'Menu',
                items: [
                    { label: 'Dashboard', icon: 'pi pi-fw pi-home', routerLink: ['/'] },
                    { label: 'Swarm', icon: 'pi pi-fw pi-share-alt', routerLink: ['swarm'] },
                    { label: 'Network', icon: 'pi pi-fw pi-wifi', routerLink: ['network'] },
                    { label: 'Pool Settings', icon: 'pi pi-fw pi-server', routerLink: ['pool'] },
                    { label: 'Settings', icon: 'pi pi-fw pi-cog', routerLink: ['settings'] },
                    { label: 'Logs', icon: 'pi pi-fw pi-list', routerLink: ['logs'] },
                    { label: 'Whitepaper', icon: 'pi pi-fw pi-bitcoin', command: () => window.open('/bitcoin.pdf', '_blank') },
                ]
            }
        ];
    }

    public restart() {
        this.systemService.restart().subscribe(res => {

        });
        this.toastr.success('Success!', 'BitForge restarted');
    }

    public isActive(routerLink: string[]): boolean {
        if (!routerLink) return false;
        return this.router.isActive(this.router.createUrlTree(routerLink), {
            paths: 'exact',
            queryParams: 'exact',
            fragment: 'ignored',
            matrixParams: 'ignored'
        });
    }

    public navigateOrExecute(item: any) {
        if (item.routerLink) {
            this.router.navigate(item.routerLink);
        } else if (item.command) {
            item.command();
        }
    }
}
