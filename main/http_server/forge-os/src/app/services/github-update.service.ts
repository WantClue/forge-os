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

  // Minimal renderer for GitHub release notes (headings, bullet lists, bold, links).
  // Input is HTML-escaped first, so only the tags produced here end up in the output.
  public renderReleaseNotes(markdown: string | null | undefined): string {
    if (!markdown) return '';

    const escape = (text: string) => text
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');

    const inline = (text: string) => escape(text)
      .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
      .replace(/\[([^\]]+)\]\((https:\/\/[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>')
      // Show PR links as "#123" like GitHub does
      .replace(/(^|[\s(])(https:\/\/github\.com\/[^\s<]+\/pull\/(\d+))/g, '$1<a href="$2" target="_blank" rel="noopener">#$3</a>')
      .replace(/(^|[\s(])(https:\/\/[^\s<]+)/g, '$1<a href="$2" target="_blank" rel="noopener">$2</a>');

    const html: string[] = [];
    let inList = false;
    const closeList = () => {
      if (inList) {
        html.push('</ul>');
        inList = false;
      }
    };

    for (const rawLine of markdown.split(/\r?\n/)) {
      const line = rawLine.trim();
      const heading = line.match(/^#{1,6}\s+(.*)$/);
      const bullet = line.match(/^[*-]\s+(.*)$/);

      if (heading) {
        closeList();
        html.push(`<h4>${inline(heading[1])}</h4>`);
      } else if (bullet) {
        if (!inList) {
          html.push('<ul>');
          inList = true;
        }
        html.push(`<li>${inline(bullet[1])}</li>`);
      } else if (line === '') {
        closeList();
      } else {
        closeList();
        html.push(`<p>${inline(line)}</p>`);
      }
    }
    closeList();

    return html.join('');
  }
}
