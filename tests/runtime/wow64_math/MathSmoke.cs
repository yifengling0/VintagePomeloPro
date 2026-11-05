using System;
using System.IO;
class MathSmoke {
 static int Root(int x,int d) {
  int lo=1,hi=x,v=0;
  while(lo<=hi){int n=lo+(hi-lo)/2;long p=1;int k=0;for(;k<d;k++){if(p>x/n)break;p*=n;}if(k<d)hi=n-1;else{v=n;lo=n+1;}}
  return v;
 }
 static int Main() {
  int errors=0,count=0;
  using(StreamWriter f=new StreamWriter("C:/windows/temp/mono-math-result.txt")) {
   for(int x=1;x<=8192;x+=1)for(int d=1;d<=8;d++) {
    double r=Math.Exp(Math.Log(x)/(double)d);
    int n=(int)Math.Floor(r);
    if(Math.Floor(Math.Pow(n+1,d))<=x)n++;
    int expected=Root(x,d);count++;
    if(n!=expected){errors++;if(errors<=20)f.WriteLine("x={0} d={1} root={2:R} n={3} expected={4} log={5:R} pow={6:R}",x,d,r,n,expected,Math.Log(x),Math.Pow(n,d));}
   }
   f.WriteLine("cases={0} errors={1} result={2}",count,errors,errors==0?"PASS":"FAIL");
  }
  return errors==0?0:1;
 }
}