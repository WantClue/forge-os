import { QuicklinkService } from './quicklink.service';

describe('QuicklinkService', () => {
  const service = new QuicklinkService();

  it('links BTC PoW Lab workers to the wallet dashboard', () => {
    expect(service.getQuickLink(
      'stratum.btcpowlab-pool.com',
      'bc1qexample.worker-one',
    )).toBe('https://btcpowlab-pool.com/miner/bc1qexample');
  });

  it('matches BTC PoW Lab hosts without case sensitivity', () => {
    expect(service.getQuickLink(
      'STRATUM.BTCPOWLAB-POOL.COM',
      'bc1qexample.worker-two',
    )).toBe('https://btcpowlab-pool.com/miner/bc1qexample');
  });
});
