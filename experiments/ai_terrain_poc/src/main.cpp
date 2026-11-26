#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
struct HeightMap {
    std::size_t width, depth;
    std::vector<float> values;
    float at(std::size_t x, std::size_t z) const { return values.at(z * width + x); }
};
struct Features { float mean, variance, slope, roughness; };
struct Parameters { float amplitude, frequency, mountains, hills, flatness, roughness; };

HeightMap sample(bool mountainous) {
    HeightMap map{32, 32, {}};
    map.values.reserve(map.width * map.depth);
    for (std::size_t z=0; z<map.depth; ++z)
        for (std::size_t x=0; x<map.width; ++x) {
            const float fx=static_cast<float>(x), fz=static_cast<float>(z);
            const float wave=std::sin(fx*0.22F)*std::cos(fz*0.19F);
            const float detail=std::sin((fx+fz)*0.83F)*0.12F;
            map.values.push_back(mountainous ? 8.0F+7.0F*wave+detail : 2.0F+0.18F*wave+detail);
        }
    return map;
}
Features analyze(const HeightMap& m) {
    if (m.width<2 || m.depth<2 || m.values.size()!=m.width*m.depth)
        throw std::invalid_argument("invalid heightmap dimensions");
    double sum=0;
    for (float h:m.values) sum+=h;
    const double mean=sum/static_cast<double>(m.values.size());
    double var=0, slope=0, rough=0;
    std::size_t edges=0, corners=0;
    for (std::size_t z=0; z<m.depth; ++z)
        for (std::size_t x=0; x<m.width; ++x) {
            const float c=m.at(x,z);
            if (x+1<m.width) { slope+=std::abs(m.at(x+1,z)-c); ++edges; }
            if (z+1<m.depth) { slope+=std::abs(m.at(x,z+1)-c); ++edges; }
            if (x+1<m.width && z+1<m.depth) {
                rough+=std::abs((m.at(x+1,z)-c)-(m.at(x,z+1)-c)); ++corners;
            }
        }
    for (float h:m.values) { const double d=h-mean; var+=d*d; }
    return {static_cast<float>(mean),static_cast<float>(var/m.values.size()),
        static_cast<float>(slope/edges),static_cast<float>(rough/corners)};
}
float clamp01(float x) { return std::clamp(x,0.0F,1.0F); }
float noise(std::uint32_t seed, std::uint32_t channel) {
    std::uint32_t v=seed+0x9E3779B9U*(channel+1U);
    v^=v>>16U; v*=0x7FEB352DU; v^=v>>15U; v*=0x846CA68BU; v^=v>>16U;
    return (static_cast<float>(v)/4294967295.0F)*2.0F-1.0F;
}
Parameters decide(const Features& f, std::uint32_t seed) {
    const float variance=clamp01(std::sqrt(std::max(0.0F,f.variance))/20.0F);
    const float slope=clamp01(f.slope/4.0F), rough=clamp01(f.roughness/4.0F);
    const float mountain=clamp01(0.65F*slope+0.35F*variance);
    const float hills=clamp01(1.0F-std::abs(0.45F-mountain)*2.0F);
    const float flat=clamp01(1.0F-0.75F*slope-0.25F*variance);
    auto vary=[&](float v,std::uint32_t c){return clamp01(v+0.035F*noise(seed,c));};
    return {vary(0.15F+0.85F*mountain,0),vary(0.15F+0.65F*(slope+rough)*0.5F,1),
        vary(mountain,2),vary(hills,3),vary(flat,4),vary(rough,5)};
}
void print(const char* label,const HeightMap& map) {
    const auto f=analyze(map); const auto p=decide(f,42U);
    std::cout<<label<<"\n  features: mean="<<f.mean<<" variance="<<f.variance
      <<" slope="<<f.slope<<" roughness="<<f.roughness
      <<"\n  profile: amplitude="<<p.amplitude<<" frequency="<<p.frequency
      <<" mountains="<<p.mountains<<" hills="<<p.hills
      <<" flatness="<<p.flatness<<" roughness="<<p.roughness<<"\n";
}
bool self_test() {
    const auto flat=analyze(sample(false)), mountain=analyze(sample(true));
    const auto a=decide(mountain,123U), b=decide(mountain,123U), c=decide(flat,123U);
    const bool same=a.amplitude==b.amplitude && a.frequency==b.frequency && a.mountains==b.mountains;
    const bool differs=a.mountains>c.mountains;
    const bool bounded=a.amplitude>=0 && a.amplitude<=1 && a.frequency>=0 && a.frequency<=1 &&
        a.mountains>=0 && a.mountains<=1 && a.flatness>=0 && a.flatness<=1;
    if (!same || !differs || !bounded) { std::cerr<<"POC smoke test failed\n"; return false; }
    std::cout<<"AI terrain POC smoke test passed.\n"; return true;
}
}
int main(int argc,char** argv) {
    if (argc>1 && std::string_view(argv[1])=="--self-test") return self_test()?0:1;
    print("Flat sample",sample(false)); print("Mountain sample",sample(true)); return 0;
}
