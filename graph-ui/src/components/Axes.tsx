import { useMemo } from "react";
import * as THREE from "three";
import { Line, Text } from "@react-three/drei";

const AXIS_LENGTH = 400;
const AXIS_ORIGIN = new THREE.Vector3(0, 0, 0);
const LABEL_OFFSET = 30;

interface AxisDef {
  dir: THREE.Vector3;
  color: string;
  label: string;
  sublabel: string;
}

const AXES: AxisDef[] = [
  {
    dir: new THREE.Vector3(1, 0, 0),
    color: "#ff4060",
    label: "X",
    sublabel: "cluster angle (cos)",
  },
  {
    dir: new THREE.Vector3(0, 1, 0),
    color: "#40ff60",
    label: "Y",
    sublabel: "cluster angle (sin)",
  },
  {
    dir: new THREE.Vector3(0, 0, 1),
    color: "#4080ff",
    label: "Z",
    sublabel: "call depth (entry → deep)",
  },
];

function AxisLine({ axis }: { axis: AxisDef }) {
  const points = useMemo(
    () =>
      [AXIS_ORIGIN, axis.dir.clone().multiplyScalar(AXIS_LENGTH)] as [
        THREE.Vector3,
        THREE.Vector3,
      ],
    [axis.dir],
  );

  const end = points[1];

  return (
    <group>
      <Line points={points} color={axis.color} lineWidth={2} />
      {/* Arrowhead cone */}
      <mesh position={[end.x, end.y, end.z]} rotation={coneRotation(axis.dir)}>
        <coneGeometry args={[6, 20, 8]} />
        <meshBasicMaterial color={axis.color} />
      </mesh>
      {/* Label */}
      <Text
        position={[
          end.x + axis.dir.x * LABEL_OFFSET,
          end.y + axis.dir.y * LABEL_OFFSET,
          end.z + axis.dir.z * LABEL_OFFSET,
        ]}
        fontSize={18}
        color={axis.color}
        anchorX="center"
        anchorY="middle"
        font={undefined}
      >
        {axis.label}
      </Text>
      {/* Sublabel */}
      <Text
        position={[
          end.x + axis.dir.x * (LABEL_OFFSET + 30),
          end.y + axis.dir.y * (LABEL_OFFSET + 30) - 16,
          end.z + axis.dir.z * (LABEL_OFFSET + 30),
        ]}
        fontSize={10}
        color={axis.color}
        anchorX="center"
        anchorY="middle"
        fillOpacity={0.5}
        font={undefined}
      >
        {axis.sublabel}
      </Text>
    </group>
  );
}

function coneRotation(dir: THREE.Vector3): [number, number, number] {
  // Cone points along +Y by default; rotate to match axis direction
  if (dir.x === 1) return [0, 0, -Math.PI / 2];
  if (dir.y === 1) return [0, 0, 0];
  if (dir.z === 1) return [Math.PI / 2, 0, 0];
  return [0, 0, 0];
}

export function Axes() {
  return (
    <group>
      {AXES.map((axis) => (
        <AxisLine key={axis.label} axis={axis} />
      ))}
    </group>
  );
}
