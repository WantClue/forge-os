import { Injectable } from '@angular/core';

@Injectable({
  providedIn: 'root'
})
export class LocalStorageService {

  constructor() { }

  setObject(key: string, value: object) {
    localStorage.setItem(key, JSON.stringify(value));
  }

  getObject(key: string): any  | null{
    const item = localStorage.getItem(key);
    if(item == null || item.length < 1){
      return null;
    }
    return JSON.parse(item);
  }

  setNumber(key: string, value: number) {
    localStorage.setItem(key, value.toString());
  }

  getNumber(key: string): number | null {
    const value = localStorage.getItem(key);
    return value ? Number(value) : null;
  }
}
