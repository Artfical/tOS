#include <iostream>
#include <vector>
int main(){ std::vector<int> v{1,2,3}; int s=0; for(int x:v) s+=x; std::cout<<"c++ ok "<<s<<"\n"; }
