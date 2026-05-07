#!/usr/bin/env python3
"""
Talicska robot — LiDAR self-filter kalibrációs script.

Beolvassa a /scan topicot, azonosítja a robot saját teste által okozott
takarásokat, és generálja a laser_filters YAML konfigurációt.

Használat (Docker-en belül):
  docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && \
    python3 /scripts/scan_self_filter_calibrate.py"

  Opciók:
    --threshold M   Takarási küszöb méterben (default: 0.50)
    --margin D      Biztonsági margó fokban (default: 3.0)
    --samples N     Átlagolás N scan-ből, robusztusabb eredmény (default: 10)
    --min-cluster K Minimum pont egy clusterhez (default: 3)
    --output FILE   Kimeneti YAML fájl (default: stdout)

Példa kimenet mentéssel:
  docker exec robot bash -c "source /opt/ros/jazzy/setup.bash && \
    python3 /scripts/scan_self_filter_calibrate.py --output /tmp/self_filter.yaml"
  docker cp robot:/tmp/self_filter.yaml \
    ~/talicska-robot-ws/src/robot/talicska-robot/config/self_filter.yaml
"""

import argparse
import math
import sys

CLUSTER_GAP_DEG = 10.0  # ennél nagyobb szögrés = új cluster


def build_trapezoid(a_start_deg, a_end_deg, r_outer, r_inner=0.03):
    """Trapéz poligon a lidar_link frame-ben, a cluster szögtartományára."""
    pts = []
    for a_deg, r in [
        (a_start_deg, r_inner),
        (a_start_deg, r_outer),
        (a_end_deg,   r_outer),
        (a_end_deg,   r_inner),
    ]:
        a = math.radians(a_deg)
        pts.append((round(r * math.cos(a), 4), round(r * math.sin(a), 4)))
    return pts


def cluster_direction(mid_angle_deg):
    lr = 'bal'  if mid_angle_deg > 0 else 'jobb'
    fb = 'elol' if -90 < mid_angle_deg < 90 else 'hatul'
    return f'{fb}_{lr}'


def analyze(scans, threshold, margin_deg, min_cluster):
    msg = scans[0]
    n_pts = len(msg.ranges)

    # Átlagolt range minden szögre (outlier-tűrő)
    avg_ranges = []
    for i in range(n_pts):
        valid = [s.ranges[i] for s in scans
                 if msg.range_min < s.ranges[i] < msg.range_max]
        avg_ranges.append(sum(valid) / len(valid) if valid else float('inf'))

    # Takarási pontok
    close_pts = []
    for i, r in enumerate(avg_ranges):
        if r < threshold:
            a_rad = msg.angle_min + i * msg.angle_increment
            a_deg = math.degrees(a_rad)
            close_pts.append({
                'angle_deg': a_deg,
                'angle_rad': a_rad,
                'range': r,
            })

    if not close_pts:
        print(f'[WARN] Nem talált takarási pontot {threshold}m-en belül.', file=sys.stderr)
        return []

    # Clusterek szögrés alapján
    clusters = []
    cur = [close_pts[0]]
    for pt in close_pts[1:]:
        if pt['angle_deg'] - cur[-1]['angle_deg'] < CLUSTER_GAP_DEG:
            cur.append(pt)
        else:
            if len(cur) >= min_cluster:
                clusters.append(cur)
            cur = [pt]
    if len(cur) >= min_cluster:
        clusters.append(cur)

    # Cluster összefoglalók
    result = []
    for cl in clusters:
        angles = [p['angle_deg'] for p in cl]
        ranges = [p['range']     for p in cl]
        a_raw_start = min(angles)
        a_raw_end   = max(angles)
        mid_angle   = (a_raw_start + a_raw_end) / 2.0
        result.append({
            'direction':     cluster_direction(mid_angle),
            'a_raw_start':   round(a_raw_start, 1),
            'a_raw_end':     round(a_raw_end,   1),
            'a_filt_start':  round(a_raw_start - margin_deg, 1),
            'a_filt_end':    round(a_raw_end   + margin_deg, 1),
            'r_min':         round(min(ranges), 3),
            'r_max':         round(max(ranges), 3),
            'r_mean':        round(sum(ranges) / len(ranges), 3),
            'r_filter':      round(max(ranges) + 0.05, 3),
            'n_pts':         len(cl),
        })
    return result


def print_report(clusters, threshold, margin_deg, samples):
    SEP = '=' * 62
    print(f'\n{SEP}')
    print('  TALICSKA LIDAR SELF-FILTER KALIBRÁCIÓ')
    print(f'  Küszöb: {threshold}m | Margó: ±{margin_deg}° | Scan átlag: {samples}')
    print(SEP)
    print(f'  Talált {len(clusters)} cluster:\n')
    for i, c in enumerate(clusters):
        print(f'  Cluster {i+1}: {c["direction"].upper()}')
        print(f'    Nyers szög:    {c["a_raw_start"]:7.1f}° → {c["a_raw_end"]:6.1f}°'
              f'  ({c["a_raw_end"] - c["a_raw_start"]:.1f}° széles)')
        print(f'    Filterrel:     {c["a_filt_start"]:7.1f}° → {c["a_filt_end"]:6.1f}°'
              f'  ({c["a_filt_end"] - c["a_filt_start"]:.1f}° széles)')
        print(f'    Távolság:      {c["r_min"]:.3f}m – {c["r_max"]:.3f}m'
              f'  (átlag: {c["r_mean"]:.3f}m, filter: {c["r_filter"]:.3f}m)')
        print(f'    Pontok:        {c["n_pts"]}')
        print()


def generate_yaml(clusters):
    lines = []
    lines.append('# Generálva: scan_self_filter_calibrate.py')
    lines.append('# Integráld a robot_bringup laser_filter launch konfigjába.')
    lines.append('')
    lines.append('scan_to_scan_filter_chain:')
    lines.append('  ros__parameters:')

    for i, c in enumerate(clusters):
        pts = build_trapezoid(c['a_filt_start'], c['a_filt_end'], c['r_filter'])
        polygon_str = ', '.join(f'({x}, {y})' for x, y in pts)
        name = f'self_filter_{c["direction"]}'
        key  = f'filter{i+1}'
        lines.append(f'    {key}:')
        lines.append(f'      name: {name}')
        lines.append(f'      type: laser_filters/LaserScanPolygonFilter')
        lines.append(f'      params:')
        lines.append(f'        polygon: "{polygon_str}"')
        lines.append(f'        polygon_frame_id: lidar_link')
        lines.append(f'        invert: false')
        lines.append(f'        # {c["direction"].upper()}: '
                     f'{c["a_filt_start"]}° → {c["a_filt_end"]}°, '
                     f'max {c["r_filter"]}m')

    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(
        description='Talicska LiDAR self-filter kalibráció',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--threshold',   type=float, default=0.50,
                        help='Takarási küszöb méterben (default: 0.50)')
    parser.add_argument('--margin',      type=float, default=3.0,
                        help='Biztonsági margó fokban (default: 3.0)')
    parser.add_argument('--samples',     type=int,   default=10,
                        help='Átlagolás N scan-ből (default: 10)')
    parser.add_argument('--min-cluster', type=int,   default=3,
                        help='Min. pont egy clusterben (default: 3)')
    parser.add_argument('--output',      type=str,   default=None,
                        help='Kimeneti YAML fájl (default: stdout)')
    args = parser.parse_args()

    import rclpy
    from rclpy.node import Node
    from sensor_msgs.msg import LaserScan

    class Calibrator(Node):
        def __init__(self):
            super().__init__('scan_self_filter_calibrator')
            self.scans = []
            self.sub = self.create_subscription(LaserScan, '/scan', self._cb, 10)
            print(f'Várakozás {args.samples} scan-re (/scan)...', file=sys.stderr)

        def _cb(self, msg):
            if len(self.scans) >= args.samples:
                return
            self.scans.append(msg)
            print(f'  [{len(self.scans)}/{args.samples}]', file=sys.stderr, end='\r')
            if len(self.scans) >= args.samples:
                print('', file=sys.stderr)
                self._finish()
                rclpy.shutdown()

        def _finish(self):
            clusters = analyze(
                self.scans, args.threshold, args.margin, args.min_cluster
            )
            if not clusters:
                return
            print_report(clusters, args.threshold, args.margin, args.samples)
            yaml_str = generate_yaml(clusters)
            sep = '=' * 62
            print(f'\n{sep}')
            print('  GENERÁLT laser_filters YAML:')
            print(sep)
            print(yaml_str)
            if args.output:
                with open(args.output, 'w') as f:
                    f.write(yaml_str)
                print(f'[OK] YAML mentve: {args.output}', file=sys.stderr)

    rclpy.init()
    node = Calibrator()
    rclpy.spin(node)


if __name__ == '__main__':
    main()
