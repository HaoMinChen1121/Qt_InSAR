#ifndef BASELINEINFO_H
#define BASELINEINFO_H

struct BaselineInfo
{
    double  perpendicular = 0; // 垂直基线 (m)
    double  parallel = 0;      // 平行基线 (m)
    double  temporal = 0;      // 时间基线 (天)
    double  slantRange = 0;    // 斜距 (m)
    double  ambiguityHeight = 0;// 模糊高程 (m/2π)
    double  incidenceAngle = 0;// 入射角 (度)
    double  lookAngle = 0;     // 视角 (度)
};

#endif // BASELINEINFO_H
