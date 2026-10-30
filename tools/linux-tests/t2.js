const dns = require('dns'), http = require('http'), https = require('https');
dns.lookup('example.com', (e, a) => {
  console.log(e ? 'FAIL dns.lookup ' + e.message : 'PASS dns.lookup ' + a);
  http.get('http://example.com/', r => { let n = 0; r.on('data', d => n += d.length); r.on('end', () => {
    console.log('PASS http.get', r.statusCode, n, 'bytes');
    https.get('https://example.com/', r2 => { let m = 0; r2.on('data', d => m += d.length); r2.on('end', () => console.log('PASS https.get', r2.statusCode, m, 'bytes')); }).on('error', e2 => console.log('FAIL https.get', e2.message));
  }); }).on('error', e1 => console.log('FAIL http.get', e1.message));
});
