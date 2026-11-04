import { NgModule } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { RouterModule } from '@angular/router';

import { PrimeNGModule } from '../prime-ng.module';
import { AppFooterComponent } from './app.footer.component';
import { AppLayoutComponent } from './app.layout.component';
import { AppMenuComponent } from './app.menu.component';
import { AppSidebarComponent } from './app.sidebar.component';
import { AppTopBarComponent } from './app.topbar.component';

@NgModule({
    declarations: [
        AppTopBarComponent,
        AppFooterComponent,
        AppMenuComponent,
        AppSidebarComponent,
        AppLayoutComponent,
    ],
    imports: [
        CommonModule,
        FormsModule,
        RouterModule,
        PrimeNGModule,
    ],
    exports: [AppLayoutComponent]
})
export class AppLayoutModule { }
