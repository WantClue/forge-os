import { HttpClient } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { Observable } from 'rxjs';
import { map } from 'rxjs/operators';


export interface GithubAsset {
  name: string;
  browser_download_url: string;
  size: number;
}

export interface GithubRelease {
  id: number;
  tag_name: string;
  name: string;
  prerelease: boolean;
  body: string;
  assets: GithubAsset[];
}

@Injectable({
  providedIn: 'root'
})
export class GithubUpdateService {

  constructor(
    private httpClient: HttpClient
  ) { }


  public getReleases(): Observable<GithubRelease[]> {
    return this.httpClient.get<GithubRelease[]>(
      'https://api.github.com/repos/wantclue/forge-os/releases'
    ).pipe(
      map((releases: GithubRelease[]) => releases.filter((release: GithubRelease) => !release.prerelease))
    );
  }

  public findAssetUrl(release: GithubRelease, filename: string): string | null {
    const asset = release.assets.find(a => a.name === filename);
    return asset ? asset.browser_download_url : null;
  }
}
