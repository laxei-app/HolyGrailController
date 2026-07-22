# -*- coding: utf-8 -*-
import http.client, json
def g(ip,p):
    c=http.client.HTTPConnection(ip,8080,timeout=4); c.request('GET',p); r=c.getresponse(); b=r.read(); c.close(); return r.status,b
for ip,tag in [('192.168.1.12','R10'),('192.168.1.4','R100')]:
    print('====',tag,ip,'====')
    for name in ['shootingmodedial','av','iso','tv']:
        try:
            st,b=g(ip,'/ccapi/ver100/shooting/settings/'+name)
            j=json.loads(b.decode('utf-8','replace'))
            val=j.get('value'); ab=j.get('ability',[])
            more='...' if len(ab)>10 else ''
            print('  %-16s value=%-10s ability(%d)=%s%s' % (name, str(val), len(ab), ab[:10], more))
        except Exception as e:
            print('  %-16s ERR %s' % (name, e))
