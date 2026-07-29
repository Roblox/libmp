# Complete MicroProfiler Scope Reference

> _Disclaimer: All information in this document is subject to change. Prepared with LLM assistance._

Scopes are timed regions on the profiler timeline — together they show where each frame's time goes. They're grouped here by engine subsystem; each entry has a short description, and many include performance notes and tips for reducing their cost.

Use Ctrl+F to search, or the Contents below to jump to a group.

<a name="contents"></a>
## Contents

- [`heartbeat.md`](#group-heartbeat) — Heartbeat Group
- [`physics.md`](#group-physics) — Physics Group
- [`simulation.md`](#group-simulation) — Simulation Group
- [`render.md`](#group-render) — Render Group
- [`gpu.md`](#group-gpu) — GPU Group
- [`shadows.md`](#group-shadows) — Shadows Group
- [`script.md`](#group-script) — Script Group
- [`network.md`](#group-network) — Network Group
- [`ui.md`](#group-ui) — UI Group
- [`animation.md`](#group-animation) — Animation Group
- [`sound.md`](#group-sound) — Sound Group
- [`input.md`](#group-input) — Input Group
- [`navigation.md`](#group-navigation) — Navigation Group
- [`voxel.md`](#group-voxel) — Voxel Group
- [`voice.md`](#group-voice) — Voice Group
- [`video.md`](#group-video) — Video Group
- [`player.md`](#group-player) — Player Group
- [`slim.md`](#group-slim) — Slim Group
- [`lc.md`](#group-lc) — LC Group (Layered Clothing / Cage Deformers)
- [`systems.md`](#group-systems) — Systems Group
- [`jobs.md`](#group-jobs) — Jobs Group (TaskScheduler)
- [`profiler-runtime.md`](#group-profiler-runtime) — Profiler & Runtime Scopes
- [`telemetry.md`](#group-telemetry) — Telemetry Group
- [`misc.md`](#group-misc) — Additional Profiler Groups

<br>

---

<a name="group-heartbeat"></a>
# 📄 `heartbeat.md` — Heartbeat Group

<sub>[↑ Contents](#contents)</sub>

The Heartbeat group contains scopes for the per-frame heartbeat step — it fires `RunService.Heartbeat` callbacks once per frame on the main thread. Heartbeat is one of several per-frame steps run by the task scheduler; physics, rendering, and scripts are separate steps documented in their own groups.

---

## heartbeatInternal

**Fires the internal heartbeat signal to all engine systems that subscribe to the per-frame heartbeat.**

This is the engine-internal heartbeat that notifies services like ProximityPromptService, PointsService, and other engine subsystems that a new frame has begun. It passes wall time, game time, and delta values to all connected listeners.

**Nesting:** Top-level scope on the main thread. All service `onHeartbeat` callbacks run inside this scope.

**Performance notes:** This is the aggregate of every engine service subscribed to the per-frame heartbeat, so a wide scope usually means one nested callback dominates — drill into the per-service scopes (e.g. `proximityPromptOnHeartbeat`) to find it. Some of this cost scales with your experience's content: for example, the number of active ProximityPrompts drives `proximityPromptOnHeartbeat` (see that scope for how to reduce it).

**What game creators can do:** Expand it and reduce the content driving the widest child (for example, fewer active `ProximityPrompt`s reduces `proximityPromptOnHeartbeat`). Note this is engine-service work — your own per-frame script code shows up under `RunService.Heartbeat`, not here.


---

## RunService.Heartbeat

**Fires the `RunService.Heartbeat` Lua event (the script-facing heartbeat callback).**

This is the Lua-facing `RunService.Heartbeat` event. Game scripts that call `RunService.Heartbeat:Connect(function)` execute here. This runs after the internal heartbeat signal.

**Nesting:** Runs immediately after `heartbeatInternal` in the same frame step.

**Performance notes:** If this scope is wide, game scripts connected to `RunService.Heartbeat` are doing expensive work. Common causes:
- Complex per-frame Lua logic (NPC AI, custom physics, UI updates)
- Too many connections to the Heartbeat event
- Heavy math or table operations in connected functions

**What game creators can do:**
- Move expensive work to `RunService.Stepped` or `RunService.PreSimulation` if it only needs to run at physics rate
- Throttle per-frame logic (run expensive operations every Nth frame)
- Use task.defer/task.spawn to spread work across frames
- Reduce the number of Heartbeat connections by consolidating logic


---

## proximityPromptOnHeartbeat

**Updates ProximityPromptService state — determines which proximity prompts are visible and calculates their display priority.**

Each frame, this scope iterates over all active ProximityPrompt instances, checks player distance, performs line-of-sight raycasts, and determines which prompts should be displayed. It handles the "best fit" selection when multiple prompts compete for screen space.

**Nesting:** Fires inside `heartbeatInternal` as a heartbeat subscriber.

**Performance notes:** Cost scales with the number of active ProximityPrompt instances in the workspace. Each prompt requires distance checks, and visible prompts require raycasts for line-of-sight verification.

**What game creators can do:**
- Reduce the number of ProximityPrompt instances in the workspace
- Set `RequiresLineOfSight = false` on prompts that don't need it (eliminates raycasts)
- Decrease `MaxActivationDistance` thresholds to reduce the pool of candidates
- Disable prompts that are far from any player


---

## pointsServiceOnHeartbeat

**Processes batched point award requests in PointsService.**

This is a legacy service scope. It processes any pending batched point awards that were queued since the last frame. In modern games this is typically a no-op.

**Nesting:** Fires inside `heartbeatInternal` as a heartbeat subscriber.

**Performance notes:** Negligible in most games. Only costs time if PointsService is actively awarding points.


<br>
<br>

---

<a name="group-physics"></a>
# 📄 `physics.md` — Physics Group

<sub>[↑ Contents](#contents)</sub>

The Physics group contains scopes for the entire physics simulation pipeline: frame orchestration, world stepping, collision detection, constraint solving, sleep management, interpolation, and per-body computations. These scopes fire on the main thread (and worker threads for parallel phases) at the fixed physics rate (60 Hz by default).

---

## Frame-Level Orchestration

### gameFixedStepped

**Top-level fixed-rate simulation frame — orchestrates humanoids, animation, scripts, and physics at 60 Hz.**

This is the outermost scope for the fixed-rate simulation step. It sequences: PreAnimation scripts → humanoid step → animation → Stepped scripts → IK → PreSimulation scripts → world step → PostSimulation scripts. Multiple sub-steps may execute per render frame if the physics rate exceeds the render rate.

**Performance notes:** If this scope is wide, identify which nested phase dominates. Common culprits: worldStep (complex physics), stepAnimation (many characters), or script callbacks (RunService.PreSimulation doing heavy work).


---

### gameStepped

**Runs the frame's RunService script callbacks at the render (variable) rate.**

Fires the per-frame `RunService.PreAnimation`, `RunService.Stepped`, and `RunService.PreSimulation` events, then resumes any scripts that were waiting. Humanoid stepping, animation, and inverse kinematics run separately in the fixed-rate step `gameFixedStepped`.

`RunService.PostSimulation` fires from the physics step (after the world step), not in this scope. (The one exception is a **paused Studio playtest** — physics is halted, so this scope fires it instead; this does not happen on live clients or servers.)

**Performance notes:** Cost is the total work your scripts do in these callbacks; drill into the individual `RunService.*` sub-scopes to see which one dominates.


---

### physicsSteppedTotal

**The complete physics step including pre/post work (ISR sync, interpolation, callbacks).**

Wraps the entire physics step from start to finish, including instance state replicator synchronization, the actual world step, and post-step notifications.

**Performance notes:** This is the "total physics bill" per frame. If consistently above budget, the scene has too much physics work.


---

### physicsStepped

**Runs the physics simulation.**

The physics simulation step nested within physicsSteppedTotal.

**What game creators can do:**
- Reduce the amount and complexity of physically simulated bodies


---

## RunService Script Callbacks

### RunService.PreAnimation

**Fires the `RunService.PreAnimation` Lua event — runs before animation stepping.**

Script callbacks connected to `RunService.PreAnimation:Connect()` execute here. Intended for game logic that must run before animation evaluation.

**Performance notes:** Cost = game script work. Keep callbacks lightweight.


---

### RunService.Stepped

**Runs the functions connected to the `RunService.Stepped` event — fires every frame, before the physics simulation.**

This is the same point in the frame as `RunService.PreSimulation`, which has superseded it. `Stepped` is still fully supported and is **not** formally deprecated, but `PreSimulation` is the event recommended for new work.

**Performance notes:** Cost is the total work your scripts do in `Stepped` callbacks. It runs before the physics step, so heavy work here delays physics for the frame.

**What game creators can do:**
- Reduce the amount or workload of functions connected to this event
- Move expensive calculations to a less frequent event, or spread them across multiple frames


---

### RunService.PreSimulation

**Fires the `RunService.PreSimulation` Lua event — runs before physics solving.**

Script callbacks for game logic that must execute before the physics world step (e.g., applying forces, setting velocities).

**Performance notes:** Expensive Lua work here directly delays the physics step. Common for custom character controllers and force application.

**What game creators can do:**
- Minimize work in PreSimulation callbacks
- Cache values instead of recomputing every frame
- Avoid raycasts or spatial queries here


---

### RunService.PostSimulation

**Fires the `RunService.PostSimulation` Lua event — runs after physics solving.**

Script callbacks for game logic that reads physics results (e.g., checking velocities, detecting collisions).

**Performance notes:** Same as PreSimulation — keep callbacks fast.


---

## Humanoid & Character Stepping

### stepHumanoid

**Steps all Humanoid instances — computes character movement, state machines, and platform standing.**

Runs the Humanoid state machine for every character in the scene. Handles running, jumping, falling, climbing, swimming, and seated states. Updates movement forces.

**Nesting:** Contains HumanoidParallelManager::stepAll or fires the humanoidSteppedSignal.

**Performance notes:** Cost scales linearly with the number of active Humanoids. Each humanoid performs raycasts for floor detection and state transitions.

**What game creators can do:**
- Reduce the number of spawned NPCs with Humanoids
- Use SimplePath or custom movement for distant NPCs instead of full Humanoids
- Set Humanoid state to Dead or disable PlatformStanding for inactive characters
- Disable Humanoid states that NPCs don't need (e.g. Climbing or Swimming)
- Reduce callbacks to Humanoid.StateChanged, or state changes such as Running or Died


---

### HumanoidParallelManager::stepAll

**Parallel dispatch of all Humanoid steps — distributes work across worker threads.**

Coordinates parallel humanoid stepping with sub-phases for wake-up, force updates, collision toggles, and synchronization.

**Nesting:** Contains: wakeUpHumanoid, forceUpdate, setPartCanCollide, syncQueue, clear.


---

### HumanoidParallelManager::stepAll::wakeUpHumanoid

**Wakes (or prevents sleep of) local humanoids each step — those owned by the local player, or locally simulating and not in a ragdoll/physics state (legacy physics path only).**


---

### HumanoidParallelManager::stepAll::forceUpdate

**Computes and applies movement forces for all active humanoids.**


---

### HumanoidParallelManager::stepAll::setPartCanCollide

**Updates collision state for humanoid body parts (e.g., disable collision during certain states).**


---

### HumanoidParallelManager::stepAll::syncQueue

**Synchronizes parallel humanoid results back to the main thread.**


---

### HumanoidParallelManager::stepAll::clear

**Sizes the per-worker collision-map buffers for the parallel humanoid step (runs before the parallel work, not after).**


---

### HumanoidParallelManager::updateHumanoidOverlaps

**Detects overlapping player Humanoids using a sweep-and-prune AABB (optionally OBB) test and tracks how long each pair has overlapped.**

**Performance notes:** Cost increases significantly with many humanoids in close proximity. Expensive with many characters in close proximity.

**What game creators can do:**
- Avoid spawning many NPCs in the same location
- Spread NPC spawn points apart


---

### HumanoidState::doAutoJump

**Evaluates auto-jump conditions — raycasts ahead of the humanoid to detect obstacles that trigger automatic jumping.**

**Performance notes:** Two raycasts per humanoid per frame when auto-jump is enabled (a torso-height ray and a jump-height ray ahead of the character).


---

### Humanoid::computeForce

**Computes movement forces for all humanoid connectors in the physics kernel this step.**


---

### humanoidPlatformsMovement

**Corrects a character's position when it is standing on a moving platform that is owned or simulated by another peer, compensating for network-induced inaccuracies.**


---

## Animation (within Physics group)

### stepAnimationPrepare

**Fires the preparation signal just before animation evaluation, letting subscribers update their inputs before poses are computed.**


---

### stepAnimation

**Evaluates all Animator instances — steps animation tracks and produces skeletal poses.**

Runs AnimatorParallelManager::stepAll (parallel) or fires the animation signal (serial) to evaluate all playing animation tracks.

**Performance notes:** Cost = (number of characters) × (playing tracks per character) × (bones per track). The biggest animation cost in most games.

**What game creators can do:**
- Limit playing animation tracks per character (stop tracks that are fully blended out)
- Use animation priorities to prevent unnecessary blending
- Reduce animation on off-screen or distant characters
- Use fewer bones in rigs when possible
- Reduce the number of animated joints to lower the workload of this step
- Reduce callbacks to animation events such as AnimationTrack.KeyframeReached or AnimationTrack.Ended


---

### AnimatorParallelManager::stepAll

**Parallel dispatch of all Animator step operations across worker threads.**


---

### stepIK

**Runs inverse kinematics solvers after animation evaluation.**

Solves all active IkControl instances. Runs after FK animation so IK can override/adjust joint positions.

**Performance notes:** See Animation group → IkControlManager::update for details.


---

### stepLegacy

**Fires the internal per-frame step signal for engine objects that step each frame (Humanoid, BodyGyro, and similar). This is not Lua script stepping — scripts connected to `RunService.Stepped` are served under the separate `RunService.Stepped` scope.**


---

### stepLegacyControllers

**Steps legacy body movers (BodyVelocity, BodyForce, BodyGyro, etc.).**

Updates all legacy body mover constraints. Modern constraint-based movers (VectorForce, LinearVelocity, AlignOrientation) are the recommended alternative.

**Performance notes:** Cost proportional to the number of active legacy body movers.

**What game creators can do:**
- Migrate from BodyVelocity/BodyForce/BodyGyro to VectorForce/LinearVelocity/AlignOrientation
- Remove body movers from parts that don't need them


---

## World Step

### worldStep

**One full physics world step — the core simulation tick including broadphase, contacts, and solving.**

The main physics step. Broadly: midphase and contact generation (narrowphase), sleep management, joint stepping, constraint solving, and finally a broadphase update to prepare the next step.

**Performance notes:** This is the core physics cost. Wide worldStep means the scene is physics-heavy. Look at nested scopes to identify bottleneck (broadphase, contacts, or solver).

**What game creators can do:**
- Reduce part count in the workspace
- Anchor parts that don't need to move
- Use simpler collision fidelity (Box instead of Default/Hull/Decomposition)
- Reduce the number of unanchored assemblies
- Set NetworkOwnership appropriately so server doesn't simulate everything


---

### Kernel::stepWorld

**Steps the physics kernel — the low-level simulation engine.**

The kernel-level world step that coordinates assembly management, contact processing, and solver dispatch.


---

### Kernel::stepWorldThrottled

**Throttled world step — runs when the engine is overloaded and unable to simulate everything in real time.**

Same as stepWorld, but when throttled only "real-time assemblies" such as Humanoids are simulated.


---

## Broadphase & Collision Detection

### updateBroadphase / adaptiveUpdateBroadphase

**Updates the spatial acceleration structure for collision detection (grid hierarchy).**

Maintains the broadphase spatial data structure used to quickly find potentially colliding pairs. Moves objects in the grid when they move in the world.

**Performance notes:** Cost proportional to the number of moving assemblies. Anchored parts have zero cost.


---

### BroadPhase and primitiveMovementCallbackCollect

**Processes the results of the completed parallel broadphase update and fires primitive movement callbacks.**


---

### BroadPhaseIslandReprocess

**Reprocesses broadphase islands when topology changes (parts added/removed).**


---

### doBroadPhaseIslandAssemblyFilter

**Filters assemblies for broadphase island assignment based on movement and spatial proximity.**


---

### doBroadPhaseParallel MT Work

**Parallel broadphase work distributed across worker threads.**


---

### postParallelBroadphase

**Post-processing after parallel broadphase — merges results from worker threads.**


---

### expandByContacts

**Expands the Ik dragger's simulation set by walking contacts outward from the dragged primitives to pull in additional primitives that must be included in the drag solve.**


---

### stepMidPhase

**Mid-phase collision detection — spatial traversal for potential contact pairs within overlapping bounds.**

Narrows down broadphase pairs using spatial acceleration structures to find specific geometry pairs that may be in contact.

**Performance notes:** Expensive with complex MeshPart collision geometry (high triangle count).

**What game creators can do:**
- Use simpler CollisionFidelity (Box, Hull) instead of Default or PreciseConvexDecomposition
- Reduce mesh complexity for parts that collide frequently


---

### MidPhase processMidPhaseStepResults

**Processes results from parallel midphase work — notifies callbacks for assembly/terrain pairs that were added or removed this step.**


---

### stepContacts / newStepContacts / newStepContacts - Adaptive

**Narrow-phase contact generation — computes exact contact points and normals between colliding shapes.**

Runs specialized contact-generation algorithms for each shape-pair type to find contact manifolds.

**Performance notes:** Cost depends on the number of active contact pairs and shape complexity. Convex decomposition parts are the most expensive.


---

### stepContactsAsyncPrepare

**Builds the list of contacts to be processed asynchronously and flags their bodies for interpolation updates.**


---

### Finalize contacts / Finalize

**Finalize contacts** applies narrowphase contact results to the physics kernel — adding, updating, or removing contact data and firing touch/untouch events. **Finalize** completes a solver batch: it applies the solver's impulse corrections, runs the final constraint iterations, and integrates the updated positions and velocities back into the simulated bodies.


---

### Finalize With Malformed Output Checks

**Integrates the solved positions into the simulated bodies while checking for numerically unstable (exploded) solver output.**


---

## Sleep Management

### preContactStepSleepStage

**Pre-contact sleep evaluation — determines which assemblies can remain sleeping this frame.**

Checks whether sleeping assemblies have been disturbed (new contacts, applied forces) and need to wake up.

**Performance notes:** Fast per-assembly check. Total cost = O(sleeping assemblies with potential contacts).


---

### postContactStepSleepStage

**Post-contact sleep evaluation — puts assemblies to sleep that have settled.**

After solving, checks whether active assemblies have come to rest (velocity below threshold for enough frames) and can be put to sleep.


---

### stepAssembliesWakePending / stepAssembliesRecursiveWakePending

**Wakes assemblies that have pending wake requests (touched by a moving object, force applied, etc.). The recursive variant also propagates waking through connected assemblies up to a bounded depth.**


---

### stepAwakeAssemblies

**Iterates over all awake assemblies to check if they should continue being awake.**


---

### sampleAwakeNonSimulatingAssemblies

**Samples awake assemblies that aren't actively simulating to check if they need to start simulating.**


---

### updateContactSleepState

**Updates the sleep state of contacts — removes contacts for sleeping pairs, activates for waking pairs.**


---

### updateVisuallySleeping

**Advances each visually-moving part's "steps to sleep" countdown and moves parts that have stopped moving into a visually-sleeping state — the second part of the moving-assemblies (`notifyMovingAssembliesAndPrimMovementCallbacks`) tracking. Parts marked visually sleeping no longer need per-frame transform updates on the render side.**


---

## Island Generation & Solver

### Generating Islands

**Partitions the awake world into independent islands for parallel solving.**

Groups connected assemblies (via contacts or joints) into independent islands that can be solved in parallel without synchronization.

**Performance notes:** Cost depends on connectivity. Highly connected scenes (many parts touching) produce fewer, larger islands that can't parallelize well.


---

### Repack Islandizer

**Compacts island data structures after topology changes.**


---

### Generating Tasks

**Creates solver tasks for each island — prepares work items for the constraint solver.**


---

### Init Batch Solve

**Initializes batch solving — prepares shared data structures for parallel island solving.**


---

### Solve Batch

**Solves a batch of constraint islands using the constraint solver.**

The main solver work for the traditional physics solver. Iterates over constraints to find forces that satisfy all contacts and joints.

**Performance notes:** This is often the most expensive physics scope. Cost depends on:
- Number of awake constraint rows (contacts + joints)
- Solver iteration count
- Island size and connectivity

**What game creators can do:**
- Reduce the number of unanchored parts
- Anchor parts that don't need physics
- Avoid large piles of parts (creates big islands with many constraints)
- Use simpler collision fidelity


---

### Solve Batch Primal

**Solves a batch of constraint islands using the alternate constraint solver.**

An alternate constraint solver that uses iterative methods for robust convergence on complex systems.


---

### Sort Constraints

**Partitions an island's constraints into interior versus exterior groups as a preparation step for the alternate (Primal) solver path.**


---

## Constraint Solver Details

### LDLPGSSolver::solve

**Main constraint solver iteration loop — iterates to convergence.**

The core constraint solver. Projects constraints iteratively to find equilibrium forces.


---

### LDL Decomp

**Decomposes the constraint matrix for efficient solving.**


---

### Apply forces

**Evaluates active force objects (such as VectorForce and the legacy body-force movers) and accumulates their force and torque onto bodies for later integration. These are user-created forces, not constraint forces, and velocities are not updated here.**


---

### ConstructLDLProgram / ConstructLDLMxVProgram

**Constructs the solver program — builds the computation graph for constraint solving.**


---

### LDLMxVProgram::applyInverse / LDLMxVProgram::applyInverseLDL

**Applies the inverse of the factored constraint matrix to a residual vector, producing impulse corrections during constraint solving.**


---

### ConstructBodyShattering

**Constructs body shattering data — splits a body with a large constraint dimension into smaller shards to reduce the cost of the direct solve.**


---

### ConstructConnectedComponent

**Identifies connected components in the constraint graph for island decomposition.**


---

### ConstructLinearForceModel

**Builds a part's aerodynamic force model — precomputing the coefficient matrices used to evaluate fluid (aerodynamic) forces from the part's surface geometry.**


---

### CreateMinimumLocalFillElimination

**Computes a fill-reducing elimination ordering for the solver by greedily eliminating the node with the fewest resulting fill-in edges.**


---

### buildLDLComponents / buildLDLProgram

**Builds the solver's factorization structures from the constraint topology: `buildLDLComponents` splits the constraint graph into independent components, and `buildLDLProgram` builds the solve plan for a single component.**


---

### constructLineGraph

**Constructs the line graph representation of constraint connectivity.**


---

### orderEdgeReductionsInPivotOrder / orderPivotsInDepthFirstOrder

**Prepares the solver's elimination ordering: `orderPivotsInDepthFirstOrder` reorders pivots by a depth-first walk of the elimination tree, while `orderEdgeReductionsInPivotOrder` orients each edge reduction so the factorization uses only the lower-triangular blocks.**


---

### applyDiagonalPreconditioner

**Applies diagonal preconditioning to improve solver convergence rate.**


---

## Alternate Constraint Solver

### Primal::solve2

**Alternate constraint solver entry point — iterates to convergence.**

An alternate solver used for complex constraint systems.


---

### Primal::newton

**One iteration of the alternate solver.**


---

### Primal::buildSystem / Primal::buildEquations

**Constructs the linearized system of equations for the solver step.**


---

### Primal::linearSolve (variants)

**Solves the linear system within a solver iteration.**

Multiple variants exist:
- `(Partition Blocks By Row, Unpack Diagonal)` — general (non-SIMD) path
- `(SIMD Partition Blocks By Row, Unpack Diagonal)` — SIMD-accelerated variant, used when the island's block layout allows it
- `(pure block-diagonal direct solve)` — fast path used when the system has no off-diagonal blocks (empty lower triangle)
- `(two-body direct solve)` — optimized for simple two-body contacts


---

### Primal::velocityStage / Primal::positionStage

**Velocity and position correction stages of the alternate solver.**


---

### Primal::explicitForces

**Applies explicit (non-constraint) forces before the solve; in the current path this is limited to buoyancy for submerged bodies, and the scope does no work when fluid forces are disabled.**


---

### Primal::buoyancy

**Computes buoyancy forces for submerged bodies in the alternate solver path.**


---

### Primal::convergence

**Checks convergence criteria — determines if the solver has reached an acceptable solution.**


---

### Primal::warmstart / Primal::warmstartPositionStage

**Initializes the solver's iteration start point. `Primal::warmstart` carries the velocity-stage solution forward (blending toward the integrated velocity) as a warm start; `Primal::warmstartPositionStage` instead zeroes the position-stage solution (a cold start).**


---

### Primal::sortLowerTriangularBlocks

**Sorts lower-triangular blocks for efficient forward/back substitution.**


---

### Primal::bodyData

**Gathers body data (mass, inertia, position) for the solver.**


---

### Primal::DualUpdateTerm / Primal::InitializeNewtonIteration

**Solver iteration steps for the primal solver — DualUpdateTerm advances the augmented-Lagrangian dual (force) variables per constraint; InitializeNewtonIteration sets up per-iteration solver state.**


---

### Primal::NewtonIterationApplyAero

**Applies aerodynamic forces within a solver iteration.**


---

### Primal::StoreOutput / Primal::StoreOutputForConstraints / Primal::StoreOutputForSimBodies

**Stores solver output — packs per-body velocity and position virtual displacements, and stores the solved constraint forces.**


---

### PrimalSolverContext::PrimalSolverContext

**Constructs the primal solver context — allocates working memory and initializes state.**


---

## Raycasting

### RaycastBatched

**Performs a batch of raycasts against the physics world (used by humanoids, sensors, scripts).**

**Performance notes:** Cost = O(rays × scene complexity). Raycasts are one of the most common script-driven physics costs.

**What game creators can do:**
- Reduce raycast frequency (don't raycast every frame if not needed)
- Use shorter ray lengths
- Use RaycastParams FilterDescendantsInstances to reduce traversal
- Avoid raycasting against complex MeshPart geometry


---

### RaycastBroadphase

**Broadphase traversal for raycasts — finds candidate objects along the ray path.**


---

### RaycastTerrain / RaycastTerrainBatched / raycastAgainstCachedTerrain

**Raycasts against Smooth Terrain voxel data.**

**Performance notes:** Terrain raycasts traverse the terrain spatial structure. Long rays across large terrain areas are expensive.


---

### Shapecast

**Performs a shape cast (swept volume test) — tests if a shape would collide moving along a path.**

More expensive than raycasts because the swept shape must test against all potential contacts, not just a thin ray.

**Performance notes:** Significantly more expensive than Raycast. Use sparingly.


---

### buildKDTree / createKDTreeAsyncAndLaunchRaycastTasks

**Builds a KDTree and launches batched raycasts used to compute mesh self-occlusion for aerodynamic force calculations.**


---

### getHitLocationPartFilterDescendents

**Filters raycast results by part hierarchy (respects FilterDescendantsInstances).**


---

### buildClientRegion3d

**Builds the local player's client-side simulation region — computes the simulation radius and replication focus used to filter physics networking.**


---

## Integration & Interpolation

### Interpolation

**Interpolates network-replicated assemblies toward their latest received state — the per-task worker inside `interpolateNetworkedAssemblies`.**

This runs in parallel across networked assemblies (bucketed so only a fraction update each step), smoothly advancing each replicated assembly and its children toward the state received over the network so remote-owned objects move smoothly between updates.

**Performance notes:** Cost = O(moving assemblies). Fast per-body (simple lerp/slerp).


---

### interpolateAfterWorldsteps

**Post-world-step interpolation — updates visual positions after all physics steps in a frame.**


---

### interpolateNetworkedAssemblies

**Interpolates network-replicated assemblies between received server states.**

Smoothly transitions physics objects from their predicted state to server-corrected state.

**What game creators can do:**
- Set the network owner of parts to the current player to reduce this, although this will usually cause more physics work to be done elsewhere


---

## Callbacks & Notifications

### notifyMovingAssembliesAndPrimMovementCallbacks

**Fires movement callbacks for all assemblies that moved this step.**

Notifies the rendering system and other listeners that assemblies have new positions.

**Performance notes:** Cost proportional to moving assemblies. If this is expensive, many parts are moving.


---

### notifyVisuallyMovingPrimitives

**Notifies the visual system about primitives that are visually moving (for rendering updates).**

**Performance notes:** Runs once per part that moved this frame; cost scales with the number of continuously-moving parts.

**What game creators can do:**
- Reduce the number of parts moving every frame; let physics bodies come to rest (sleep) instead of jittering.
- Weld groups of parts into a single assembly so they move as one unit rather than many.
- When moving things from scripts, move a container/model rather than many individual parts.


---

### primitiveMovedCallback / onPrimitiveMovementCallbacks

**Per-primitive movement callback — updates rendering, streaming, and other systems.**


---

### movedPrimitives / collectPrimitivesFromMovingAssemblies

**Processes primitives that moved during parallel interpolation — fires post-move updates and wakes touching assemblies.**


---

### prepareCallbacks

**Prepares callback data structures before firing movement notifications.**


---

### WorldStepSignal

**Fires the world step signal — notifies all world step observers.**


---

### fireStateChangeEvents

**Fires Humanoid state change events (e.g., Landing, Freefall, Running transitions).**


---

### queueJointGuidsForPropChangedSignals

**Queues property-changed signals for joints that were modified by the solver.**


---

## Terrain & Deferred Updates

### applyDeferredTerrainChanges / applyDeferredUpdates

**Applies queued terrain modifications to the physics representation.**

When terrain is edited, changes are batched and applied during the physics step to maintain consistency.


---

### deletedTerrainChunks / updatedTerrainChunks

**Processes removed and updated terrain chunks for the physics representation.**


---

### cacheCurrentTerrain / readGrid

**Reads and caches terrain voxel data for physics queries.**


---

### testClearTerrainWeldsForGatheredCells

**Clears terrain weld constraints for cells that have been modified.**


---

## Assembly Management

### assemble

**Updates a tree of connected objects (assemblies) used by the physics engine.**

**What game creators can do:**
- Reduce the amount of joints being created or destroyed


---

### GatherAssemblies

**Gathers the actively-simulating assemblies (and, when not throttling, moving dynamic assemblies) into the broadphase set for this step.**


---

### processPendingCollisionAssemblies

**Processes assemblies with pending collision changes — CanCollide changes, geometry/CollisionFidelity changes, size changes, and NoCollision joint add/remove.**


---

### ResetBroadIslandForAssembly

**Resets the broadphase island assignment for an assembly that changed connectivity.**


---

### getAdaptiveBodiesInSolver

**Retrieves bodies that use adaptive simulation quality (LOD for physics).**


---

### computeMassAndIntertia

**Computes mass properties (centroid, volume, inertia tensor) for a single mesh-based part from its own mesh geometry.**


---

## Aerodynamics

Building a body's aerodynamic model (the `*Construction` scopes below and `generateAeroMesh`) is one-time setup — it runs when a part gains fluid forces or its mesh changes, not every frame. For MeshParts and unions this setup cost grows with the mesh's triangle count, so very high-poly meshes take longer to prepare. The per-frame aerodynamic force itself runs on a reduced (simplified) model, so its cost stays bounded regardless of the source mesh's complexity.

### AerodynamicInterpolatorConstruction

**Constructs the aerodynamic force interpolator for wind/drag simulation.**


---

### ReducedMeshAeroForceModelConstruction

**Builds a reduced mesh model for efficient aerodynamic force computation.**


---

### extractAeroMeshFromPrimitive

**Extracts the aerodynamic mesh surface from a primitive shape for drag calculation.**


---

### generateAeroMesh

**Generates the full aerodynamic mesh for a body.**


---

### TurbulenceCoefficientsUpdate

**Updates turbulence coefficients for wind simulation.**


---

### getOccludedMeshDataMTLimited

**Computes wind occlusion data for aerodynamic surfaces (which faces are sheltered).**


---

## Sensors & Force Computation

### BuoyancyAccumulator::computeForce

**Applies the buoyancy force to each body that is in water (floating or submerged).**

Each body's buoyancy force for the frame is computed earlier; this step just adds that force into the physics solver for every buoyant body. The per-body cost is constant — it does **not** depend on how complex the body's shape is.

**Performance notes:** Cost scales with the *number* of buoyant bodies (parts touching water), not their shape complexity. The physics step shows a `buoyancyAccumulators: N` label with this count. Normally cheap; only notable when a very large number of parts are in water at once.

**What game creators can do:**
- Reduce the number of parts simultaneously in water


---

### KernelJoint::computeForce

**Computes constraint forces for one joint (Motor6D, HingeConstraint, etc.).**


---

### AtmosphereSensor / BuoyancySensor / ControllerPartSensor / FluidForceSensor

**Sensor scopes for various force-computation systems — gather environmental data for force calculations.**


---

## Miscellaneous

### workspaceOnHeartbeat

**Workspace heartbeat handler — performs per-frame physics housekeeping.**


---

### handleFallenParts

**Detects and destroys parts that have fallen below the workspace's FallenPartsDestroyHeight.**

**Performance notes:** Checks all moving parts against the destroy threshold. Normally fast.

**What game creators can do:**
- Lower the destroy height or reduce the amount of parts that fall to the destroy height


---

### updateBones

**Updates dirty attachment-hierarchy constraints after physics solving — refreshes bones and attachment-attached constraints.**


---

### updatePhysicsInstructions

**Updates physics instruction state for adaptive simulation.**


---

### updatePartCollisions

**Recomputes character part collision data when avatar scaling changes (R15 scaling defaults).**


---

### SafeMove / MoveCoarse / MoveFine

**Kinematic movement functions — move parts safely without tunneling through geometry.**

SafeMove is the high-level entry point; MoveCoarse does large steps; MoveFine does sub-step refinement.


---

### collectCharacterParts

**Collects all parts belonging to a character model for physics grouping.**


---

### Batch Expansion

**Reserves and populates an island batch's working buffers (sim bodies, anchored bodies, constraints) for the current solver step.**


---

### ContactManagerPhysicalPropertiesChanged

**Handles physical property changes (friction, elasticity) on existing contacts.**


---

### ContactManagerOnAssemblyAdded / ContactManagerOnAssemblyRemoving

**Handles assembly addition/removal from the contact manager.**


---

## ISR-Related (Instance State Replication)

### ISR-PhysicsStep::notifyMovingNOURoots

**Notifies the Instance State Replicator about moving Network Owner Unit roots for replication.**


---

### ISR-WorkspaceSynchHelper::processQueuedSynchData

**Processes queued synchronization data from the ISR system for physics state.**


---

### SpatialFilter::filterStep

**Updates simulation islands, arranging parts according to network ownership and local simulation. Islands are non-interacting groups of parts which can be simulated independently.**

**What game creators can do:**
- Avoid setting network ownership frequently
- Keep groups of parts far enough away from each other so they can be simulated separately


---

## UpdateControllers

**Steps all constraint controllers (VectorForce, LinearVelocity, AlignOrientation, etc.).**

Evaluates modern constraint-based body movers and applies their forces/velocities to the solver.

**Performance notes:** Cost proportional to active constraints. Generally well-optimized.


---

## MeshAssembly-Task / MeshProcessing-Task

**Builds a body's aerodynamic mesh data on a worker thread (used by fluid-force / buoyancy models), not collision geometry.**

Runs asynchronously on worker threads to build/process a body's aerodynamic mesh data (e.g. triangle-set occlusion weighting), not collision meshes. Fires when a part gains fluid forces or its mesh changes.


---

## generateCollisionGeometry

**Generates the collision representation for a part (e.g. convex decomposition of a mesh) used by the physics broad/narrowphase.**

**Performance notes:** One-time cost per unique mesh. Cached after generation.


---

## generateShape / generate

**Generates the collision mesh for a Terrain chunk (smooth-voxel geometry).**

**Performance notes:** Runs when terrain is edited or streamed in; cost scales with the amount of terrain being (re)meshed.


---

## Shapecast & Raycast Details

### Aerodynamic Integrator

**Integrates rigid body velocities with aerodynamic force models.**


---

### LadderRaycast

**Raycast used to detect ladder surfaces for character climbing state transitions.**

**Performance notes:** One spatial query per climbing check. Cost rises with dense collision geometry in the character's vicinity.


---

### Kernel::stepIkSolver

**Executes inverse kinematics solving on connectors for kinematic chains.**


---

### NOUSignals

**Processes deferred Network Ownership Unit (NOU) signals and joint change callbacks for ownership and spanning tree modifications.**


---

### PGS Solve

**Executes iterative constraint solving on contacts and collisions.**


---

### Primal::linearSolve (Partition Blocks By Row, Unpack Diagonal)

**Solves the linear system using row-partitioned blocks with diagonal unpacking.**


---

### Primal::linearSolve (SIMD Partition Blocks By Row, Unpack Diagonal)

**SIMD-accelerated variant of the row-partitioned linear solve — used when the island's diagonal blocks can be processed with SIMD.**


---

### Primal::linearSolve (pure block-diagonal direct solve)

**Direct solve for block-diagonal systems — fast path for simple constraint topologies.**


---

### Primal::linearSolve (two-body direct solve)

**Optimized direct solve for two-body contact constraints.**


---

### applyInnerBox

**Updates part collision data for avatars to enforce inner box collision constraints based on R15 scaling models.**


---

### applyJointTransforms

**Applies kinematic joint transformations to moving assemblies and updates their primitive positions in parallel.**


---

### primitiveMovementCallbacks

**Invokes movement callbacks for primitives whose extents changed due to assembly operations or joint transformations.**


---

### stepIk

**Executes the IK dragger physics step including contact stepping and solver updates for IK-controlled bodies.**


---

### stepUiLegacyJoints

**Updates legacy kinematic joints (VehicleSeat, SkateBoardPlatform, DynamicRotateJoint) using their stepUi callbacks.**


<br>
<br>

---

<a name="group-simulation"></a>
# 📄 `simulation.md` — Simulation Group

<sub>[↑ Contents](#contents)</sub>

The Simulation group contains scopes for the **Server Authority** simulation system (opt-in, currently in Beta). Under Server Authority the server is the source of truth: the client simulates its own inputs slightly ahead of the server (client-side prediction) and rolls back and re-simulates when the server's authoritative state differs from the prediction. These scopes appear on the client when a game has the Server Authority beta feature enabled. (`AuroraService`/`Aurora*` is the internal name for this system, so the scope strings carry that prefix.)

---

## AuroraService::stepPhysics

**Performs one fixed-rate physics step within the Aurora simulation framework.**

Advances the Aurora physics world by one fixed-rate step: it fires the step signal and then runs the physics world step (stepPhysicsRuntime). Input application and script execution are sequenced by the outer `AuroraService::onWorldFixedRateTick`, not by this scope. Called at the fixed physics rate (typically 60 Hz).

**Performance notes:** Duration depends on scene complexity and the number of simulated bodies. If this scope is wide, the physics world step (nested inside) is the likely culprit.


---

## AuroraService::onWorldFixedRateTick

**Top-level entry point for a single Aurora fixed-rate tick — orchestrates input, scripts, and physics for one simulation frame.**

This is the outer wrapper that sequences the entire fixed-rate simulation step: collecting inputs, firing script signals, stepping physics, and updating predicted state.

**Performance notes:** If consistently wide, check the nested scopes (Update Inputs, fixedRateTickSignal) to identify which phase is expensive.


---

## Update Inputs

**Applies queued player inputs to the simulation state for the current tick.**

Processes the input buffer, applying all inputs that are scheduled for this simulation frame. This includes movement inputs, button presses, and other player actions.

**Performance notes:** Typically fast. Only expensive if many players are sending high-frequency inputs.


---

## fixedRateTickSignal

**Fires the fixed-rate tick signal to all Lua scripts connected to Aurora's fixed-step callback.**

Invokes game scripts that have registered for the fixed-rate simulation callback. This is the script execution phase within each fixed-rate tick.

**Performance notes:** Cost depends entirely on what game scripts do in their fixed-step handlers. Same optimization advice as RunService callbacks.

**What game creators can do:**
- Keep fixed-step script logic lightweight
- Avoid heavy operations (raycasts, spatial queries) every tick


---

## AuroraService::prunePredictedInstances

**Removes predicted instances that are no longer relevant to the current simulation state.**

Cleans up instances that were speculatively created during prediction but are no longer needed (either confirmed by server or invalidated by rollback).

**Performance notes:** Typically fast. Can spike during rollback scenarios with many predicted objects.


---

## AuroraService::getCurrentInputFrame

**Retrieves the current input frame data for the local player.**

Reads the latest input state for the current simulation frame on the client, for client-side prediction.

**Performance notes:** Negligible — simple data lookup.


---

## AuroraService::applyInput

**Applies the input frame matching a given world-step id (all of that frame's actions) to the simulation, on the client or server.**

Takes one input entry and applies its effects to the game state (e.g., character movement, action triggers).

**Performance notes:** Fast per-call, but called for each input in the frame's input list.


---

## AuroraService::applyInputFrame

**Applies an entire frame's worth of inputs at once.**

Batch-applies all inputs for a given simulation frame. This is the primary input application path.

**Performance notes:** Cost proportional to the number of inputs queued for the frame.


---

## AuroraService::rollback

**Rolls back the simulation to a previous state when the server's authoritative state disagrees with the client's prediction.**

Reverts the world state to a known-good server snapshot and prepares to resimulate forward. This involves undoing physics changes, script state, and instance modifications.

**Performance notes:** Rollbacks are expensive — they undo and redo simulation work. Frequent rollbacks indicate prediction/authority disagreements. If this scope appears often:
- Network latency may be high
- Client and server simulations may be diverging frequently
- Complex scenes with many interacting physics bodies increase rollback cost

**What game creators can do:**
- Reduce the number of physically-simulated network-owned parts
- Ensure deterministic behavior in simulation scripts
- Minimize interactions between client-predicted and server-owned objects


---

## AuroraService::resimulate

**Re-runs the simulation forward from the rollback point to the current time.**

After a rollback, this scope replays all simulation steps from the rollback point to "catch up" to the present. Each replayed step re-applies inputs and re-runs physics.

**Performance notes:** Cost = (number of frames to replay) × (cost of one simulation step). Longer rollbacks (higher latency) and more complex scenes make this more expensive.

**What game creators can do:**
- Same as rollback — reduce scene complexity for server-authoritative objects
- Lower network latency reduces the number of frames that need resimulation


---

## AuroraService::checkServerViewAndRollback

**Compares the client's predicted state against the server's authoritative state and triggers a rollback if they disagree.**

This is the prediction validation step. It checks whether the server's latest acknowledged state matches what the client predicted. If not, it initiates a rollback and resimulation.

**Performance notes:** The comparison itself is fast. The expensive part is if it triggers a rollback.


---

## fixedStepCallbacks

**Fires all callbacks registered for the fixed-step simulation rate.**

Invokes engine-internal callbacks that run at the fixed simulation rate. This includes custom fixed-step logic and module callbacks.

**Performance notes:** Typically fast unless modules register expensive fixed-step work.


---

## RunService::getCurrentInputFrame

**Reads the current input frame from the ContextActionService for server-authority input routing.**

Bridge between RunService and the input system — retrieves the local input state for the current frame in the context of Aurora simulation.

**Performance notes:** Negligible.


---

## ContextActionService::applyInputFrame

**Applies an Aurora input frame through the ContextActionService action routing system.**

Routes an input frame through the bound action system, triggering any ContextActionService actions that match the inputs.

**Performance notes:** Cost depends on the number of bound actions and whether they trigger Lua callbacks.


---

## ContextActionService::applyInput

**Applies a single input through ContextActionService action routing.**

Processes one input event through the action binding system.

**Performance notes:** Fast per-call.


---

## UpdatePredictedHashes

**Computes and stores hashes of predicted simulation state for later comparison against server state.**

Generates hash values of the current predicted world state. These hashes are later compared against server-provided hashes to detect prediction mismatches that require rollback.

**Performance notes:** Cost scales with the number of network-replicated physics objects.


<br>
<br>

---

<a name="group-render"></a>
# 📄 `render.md` — Render Group

<sub>[↑ Contents](#contents)</sub>

The Render group contains scopes for the CPU-side rendering pipeline: scene preparation, culling, draw call building, pass dispatch, post-processing setup, and presentation. GPU-side passes are tracked separately in the GPU group.

Many scopes appear in pairs: one in "Render" (CPU submission) and one in "GPU" (GPU execution).

---

## Frame-Level Entry Points

### RunService.RenderStepped

**Fires the `RunService.RenderStepped` Lua event — game scripts that need to run before rendering.**

`RunService.PreRender` is the recommended alternative. Scripts connected to RenderStepped execute here.

**Performance notes:** Same as any script callback. Keep lightweight.


---

### RunService.PreRender

**Fires the `RunService.PreRender` Lua event — the preferred pre-render callback.**

Modern replacement for RenderStepped. Game scripts that update visual state before rendering run here.

**What game creators can do:**
- Only put visual updates here (camera, UI state)
- Avoid physics or logic work (use PreSimulation instead)


---

### RenderSteppedInternal

**Internal engine render-step callback — updates engine visual systems before scene traversal.**

Engine-internal work that prepares the rendering state: camera updates, light parameter updates, particle system advances, etc.


---

### fireBindToRenderStepCallbacks

**Fires callbacks registered via `RunService:BindToRenderStep()` — priority-ordered pre-render callbacks.**

Invokes all callbacks registered with specific priorities. These run in priority order (lower number = earlier).

**Performance notes:** Cost = sum of all BindToRenderStep callbacks. If wide, specific callbacks are doing heavy work.

**What game creators can do:**
- Unbind render step callbacks when not needed
- Keep bound callbacks minimal (only visual updates)
- Use appropriate priorities to avoid unnecessary ordering


---

## Scene Preparation (Prepare Phase)

### Prepare

**Top-level preparation scope — updates the scene graph before rendering.**

Orchestrates all pre-render scene graph updates: coordinate frame syncing, part invalidation, lighting preparation, and cluster updates.

**Nesting:** Contains SceneUpdater scopes and UI Layout.


---

### Perform

**When actual rendering commands are created and issued.**


---

### SceneUpdater::UpdatePrepare

**Prepares scene state — processes pending part additions, removals, material changes, and attachments.**

Entry point for scene graph maintenance. Handles:
- New parts entering the scene
- Parts with changed materials
- Terrain updates
- Attachment constraint processing

**Performance notes:** Cost proportional to scene changes this frame. Static scenes are nearly free.

**What game creators can do:**
- Avoid creating/destroying many parts every frame
- Batch property changes rather than changing properties one at a time
- Reduce unique material count


---

### SceneUpdater::updateDynamicParts

**Updates the coordinate frames (world matrices) for all dynamic parts each frame — including Beams, ParticleEmitters, Trails, Lights, and Humanoids — so their rendering follows their latest positions.**

**Performance notes:** Cost scales with the number of visible dynamic parts (Beams, ParticleEmitters, Humanoids, etc.).

**Labels:** `"Total parts: {N}"` — shows the number of parts being updated.

**What game creators can do:**
- Reduce the number of visible Beams, ParticleEmitters, and Humanoids

---

### SceneUpdater::performUpdateCoordinateFrame / SceneUpdater::preparePerformUpdateCoordinateFrame

**Updates the GPU-visible coordinate frames (world matrices) for dynamic FastCluster parts.**

Writes updated transform matrices to the buffers that the GPU will read for rendering.

**Performance notes:** Cost proportional to moving objects. One matrix write per moving entity.


---

### SceneUpdater::computeLightingPerform / SceneUpdater::computeLightingPrepare

**Computes dynamic lighting — updates light grid, shadow parameters, and environment lighting.**

Prepares all lighting data needed for the current frame: updates light positions, recomputes the light grid, processes new/removed lights.

**Performance notes:** Cost depends on the number of dynamic lights. Many PointLights/SpotLights increase this.

**What game creators can do:**
- Reduce the number of dynamic lights
- Use static lighting where possible
- Set light `Enabled = false` for lights that aren't visible
- Move the camera less to reduce lighting recalculation near the camera


---

### SceneUpdater::updateInstancedClusters2

**Updates instanced cluster rendering state — the batching system for similar parts.**

Maintains the instanced rendering batches: adds/removes parts from clusters, updates transforms within clusters, and handles cluster splits/merges.

**Performance notes:** Cost depends on the number of cluster membership changes. Static scenes are free.

**Labels:** `clusters: {N}` — the number of instanced clusters updated.

**What game creators can do:**
- Reduce work that implicitly updates a part's bounding box (`BasePart.CFrame`, `BasePart.Size`, `Motor6D.Transform`)
- Prefer property updates that don't move the bounding box, such as `Bone.Transform`, where possible

---

### updateInvalidatedFastClusters / updateInvalidParts / updateWaitingParts

**Processes parts that were invalidated (material change, resize, etc.) and need visual updates.**

**updateInvalidParts** updates parts that had some property changed or added. **updateInvalidatedFastClusters** prepares "FastCluster" geometries used to render Humanoids and skinned MeshParts (labels specify the number of parts, vertices, and size of vertices).

**What game creators can do:**
- Reduce the amount of property changes on the world; if a script updates a large set of object properties, break it down across frames
- Reduce visual changes to models with Humanoids or skinned MeshParts


---

### sortObjects

**Sorts the render queue by material, distance, or Z-index for optimal draw order.**

Multiple calls per frame for different passes (main, depth, highlight).

**Performance notes:** O(n log n) where n = visible objects. Fast with modern sorting.


---

## Culling

### SpawnCullJobs

**Spawns frustum culling work on worker threads — determines which objects are visible.**

Kicks off parallel frustum culling: tests all potentially-visible objects against the camera frustum.


---

### CullJob

**One culling thread's work — tests a batch of objects against the view frustum.**

Per-thread culling computation. Tests bounding boxes/spheres against the frustum planes.

**Performance notes:** Cost = O(total objects / thread count). More objects = more culling work.

**What game creators can do:**
- Reduce the total number of parts/meshes in the workspace
- Use StreamingEnabled to limit loaded objects
- Group small objects into fewer larger ones (Union, MeshParts)


---

### CullableSceneUpdate

**Updates the cullable scene representation — maintains spatial acceleration structures for culling.**


---

### CullView

**Culls one view (camera) — produces the visible set for that view.**


---

### WaitOnRenderNodeQueryFence

**Waits for culling to complete — synchronizes the main thread with cull worker threads.**

The main thread blocks here until all culling work is done. If this scope is wide, culling is the bottleneck.

**Performance notes:** If consistently long, there are too many objects to cull or too few threads.


---

## Render Queue & Pass Building

### updateRenderQueue

**Updates render queue groups for all passes — assigns visible objects to their render pass slots.**

After culling determines visibility, this scope assigns objects to specific render passes (depth, opaque, transparent, etc.).

**Performance notes:** Cost scales with the number of visible objects and the number of distinct passes/materials they map to.

**What game creators can do:**
- Reduce the number of objects visible on screen (lower draw distance, fewer parts).
- Reduce the number of distinct materials so more objects group into the same pass.


---

### BuildView / BuildDraws

**Builds the draw call list for a view — creates GPU command data.**

Converts the high-level render queue into actual draw commands (shader binding, mesh binding, constant buffer setup).

**Performance notes:** Cost proportional to visible draw calls. Complex materials and many unique meshes increase cost.


---

### BuildPass.BatchAndSort

**Batches and sorts draw calls within a pass for state-change minimization.**

Groups draw calls by shader/material to minimize GPU state changes.


---

### Dispatch / DispatchScene.beginRender / DispatchScene.refreshMeshSubsets

**The render-command dispatch phase. `Dispatch` is the actual CPU→GPU submission of draw calls; `DispatchScene.beginRender` sets up per-frame state (clears frame data, refreshes technique caches, applies mip results) and `DispatchScene.refreshMeshSubsets` refreshes CPU-side mesh LOD/subset tables — neither submits GPU commands.**


---

### DispatchSceneFRMBudget

**Frame Rate Manager budget calculation for the legacy-pipeline emulation path.**

The FRM evaluates whether the current frame met its time budget and adjusts quality settings accordingly.

**Performance notes:** If rendering is over budget, FRM reduces quality (lower resolution, fewer effects).


---

## Main Render Passes

### DepthPrePass

**Depth-only pre-pass — renders opaque, terrain, and opaque-caster geometry to populate the depth buffer without color writes. Alpha-tested opaque geometry is handled separately by the OpaqueWithAlpha depth passes.**

Enables early-Z rejection for the main color pass, significantly reducing overdraw.

**Performance notes:** Cost proportional to visible opaque geometry vertex count.

**Labels:** `"NodeCount: {N}"` — number of objects rendered in this pass.

---

### OpaqueWithAlphaDepthPass / OpaqueWithAlphaLateDepthPass

**Depth pass for objects with alpha-tested materials (foliage, fences, etc.).**

Objects with transparency that still need depth-testing get separate depth passes.

**Labels:** `"NodeCount: {N}"`

---

### depthPrepass / alphaPrepass / mainView

**CPU-side scope for the respective depth and main render passes within UpdateView.**

**Labels:** `"NodeCount: {N}"`

---

### Scene

**Main 3D scene rendering (CPU side) — traverses the visible scene and submits the draw calls for its opaque and transparent geometry.** This measures the CPU cost of preparing the frame's 3D render; the matching GPU execution time is the GPU-group `Scene` scope.

**Performance notes:** Usually the largest CPU rendering cost. Driven mainly by:
- Number of draw calls — each unique mesh × material combination is a separate call
- Part / instance / object count that must be culled and submitted each frame
- Decal and adornment count (each adds submissions)

**What game creators can do:**
- Reduce the number of *unique* mesh × material combinations so more objects batch into fewer draw calls
- Reduce overall part / instance and decal count
- Reuse the same meshes and materials instead of many one-off variants
- Shading-side cost — material/shader complexity, transparency and overdraw, resolution — shows up on the GPU-group `Scene` scope, not here


---

### Scene2D

**2D screen-space rendering — draws the screen-space UI render queues (screen-space GUIs and adornments).**


---

### GuiScene

**ViewportFrame rendering — renders 3D content within ViewportFrame UI elements.**

Each ViewportFrame is a mini-scene that gets its own render pass.

**Performance notes:** Each ViewportFrame adds a full render pass. Many ViewportFrames are expensive.

**What game creators can do:**
- Minimize the number of visible ViewportFrames
- Reduce object complexity within ViewportFrames
- Set ViewportFrame.LightDirection to avoid dynamic lighting


---

### Highlight

**Highlight effect pass — populates the CPU-side render queues for the Highlight instance's passes. The actual GPU rendering of highlighted objects happens later in the frame.**


---

### Clear

**Clears the framebuffer at the start of rendering.**

**Performance notes:** Nearly free — single GPU operation.


---

### UI

**Renders all 2D UI elements (ScreenGuis, adorns, text, images).**

Draws all on-screen GUI elements including text, images, frames, and other UI primitives.

**Performance notes:** Cost depends on the number and complexity of visible UI elements. Deep UI hierarchies with many elements increase draw call count.

**What game creators can do:**
- Reduce visible GuiObject count
- Use CanvasGroups to batch UI rendering
- Set Visible = false on off-screen elements
- Avoid excessive use of UIGradient (adds draw calls)


---

## Post-Processing Effects

### HBAO

**Screen-space ambient occlusion — screen-space AO computation.**

Computes ambient occlusion from the depth buffer to add contact shadows and depth perception.

**Performance notes:** Full-screen effect. Cost depends on resolution and quality setting. The FRM may reduce AO quality under pressure.


---

### Glow

**Bloom/glow post-processing — extracts bright areas and blurs them for glow effect.**

**What game creators can do:**
- Reduce the number of post-processing effects; usually not significant

**Performance notes:** Multi-pass blur. Usually not a bottleneck but contributes to post-processing budget.


---

### DOF / Depth of Field

**Depth of Field effect — blurs areas that are out of the camera's focus range.**

**Performance notes:** Full-screen effect. Heavier than glow due to depth-dependent kernel.


---

### BlurFx

**Blur effect — general-purpose screen blur (not DOF-specific).**

Used for the BlurEffect instance that game creators can add to the camera.


---

### SunRays

**Sun shaft (god ray) effect — computes volumetric light scattering from the sun.**

**Performance notes:** Full-screen radial blur. Moderate cost.


---

### ColorCorrection / Image Composition

**Final image composition — applies color correction, tonemapping, and combines all layers.**

The final pass that produces the output image: combines the lit scene with post-effects, applies tonemapping, and color grading.

**What game creators can do:**
- Reduce the number of post-processing effects; usually not significant


---

### VignetteEffect

**CPU-side update of the vignette effect parameters (aperture based on camera motion). The vignette darkening itself is applied later during image composition.**


---

### MSAA

**MSAA resolve pass — resolves multi-sample targets to single-sample for post-processing.**

**What game creators can do:**
- Reduce the number of post-processing effects; usually not significant


---

### DownSample

**Downsamples the framebuffer for lower-resolution effects (bloom, AO).**


---

## Upscaling

### Upscale SmootherStep / Upscale Lanczos / Upscale Linear

**Resolution upscaling — renders at lower resolution and upscales for performance.**

Different upscaling algorithms with quality/performance tradeoffs:
- Linear: fastest, lowest quality
- SmootherStep: balanced
- Lanczos: high quality, more expensive


---

## Shadows

### renderShadowMap

**Renders the shadow map atlas — generates depth maps from light perspectives.**

See the Shadows group for detailed shadow system scopes.


---

### Shadow Blur

**Blurs shadow maps for soft shadow edges.**


---

## Terrain & Virtual Texturing

### updateTerrainPrepare / updateTerrainPerform

**Prepares and executes terrain rendering updates (LOD transitions, texture streaming).**


---

### TerrainFeedbackJittered / TerrainFeedbackTiled

**Virtual texture feedback passes — determines which terrain texture tiles to load.**

Renders a feedback buffer that tells the texture streaming system which virtual texture tiles are needed at what resolution.


---

## Environment & Sky

### AdvSky / AdvSky/Compute

**Renders the advanced sky — skybox with atmospheric scattering (sky color, haze, glare). AdvSky/Compute is a background-thread step that tessellates the sky mesh and computes per-vertex scattering colors; it does no GPU rendering. Volumetric clouds are rendered separately (see Clouds / CloudsComp).**


---

### Clouds / CloudsComp

**Renders volumetric clouds.**

**Performance notes:** Can be expensive depending on cloud density and quality settings.


---

### EnvCapture

**Captures the environment map — renders a cube map for reflections.**

Periodically re-renders the environment for reflection probes.

**Performance notes:** Renders one face of the reflection cube map per invocation; the six faces are amortized across multiple frames (one face per frame).


---

## Mesh & Texture Management

### DynamicGeometryManager::processPendingRequests

**Processes pending geometry requests — loads/decodes mesh data for rendering.**

Processes queued EditableMesh updates (mesh edits submitted through the EditableMesh API) and uploads their vertex/index data to GPU buffers.


---

### DynamicGeometryManager::prepareMeshUpdates / prepareMeshUpdates::perEditableMesh

**Prepares mesh updates — processes EditableMesh changes and mesh LOD transitions.**


---

### DynamicGeometryManager::garbageCollectIncremental

**Incrementally garbage-collects unused mesh GPU resources.**


---

### DynamicGeometryManager::prepareConsumerUpdates / reallocateEditableMeshBinding

**Updates mesh consumer state and reallocates bindings for EditableMesh changes.**


---

### DynamicGeometryManager::transcodeVerticesAndCalculateBounds (and variants)

**Transcodes mesh vertices from asset format to GPU format and computes bounding boxes.**

Variants include: DynamicGeometryManager::transcodeBoxMappedVerticesAndCalculateBounds, DynamicGeometryManager::transcodeFaceFilteredVerticesAndCalculateBounds.


---

### TextureManager::processPendingRequests / DynamicTextureManager::prepareTextureUpdates / DynamicTextureManager::garbageCollectIncremental

**Texture management — loads textures, prepares updates, and garbage-collects unused textures.**


---

### MeshManager::processPendingRequests

**Processes pending mesh loading requests.**


---

### MeshJob

**Render-thread mesh upload — pops decoded mesh jobs from the upload queue and uploads their geometry to the GPU.**


---

### CheckAndGetAssets

**WrapDeformer (layered clothing) asset check — requests the WrapDeformer's body-part and cage meshes, initiating loading for any that are missing.**


---

## Fast Clusters & Instanced Rendering

### updateInvalidatedFastClusters

**Updates fast clusters that were invalidated (part moved, material changed, etc.).**

Fast clusters are the instanced rendering batches. When a part in a cluster changes, the cluster must be rebuilt.

**Performance notes:** Cost proportional to invalidated clusters. Frequent part property changes cause frequent rebuilds.

**What game creators can do:**
- Avoid changing Color/Material/Size of parts every frame
- Batch visual changes together
- Use BeamEffects or Particles instead of rapidly-changing parts


---

### BroadcastInvalidations

**Broadcasts rendering invalidations to all affected subsystems.**


---

## UI Rendering (within Render group)

### UpdateStyleQueries / UpdateUILayouts

**Updates CSS-like style queries and UI layout computations.**

**Performance notes:** Layout is expensive with deep or complex UI hierarchies. Cost scales with the number of GUI objects laid out and with the number and nesting of layout objects (`UIListLayout`, `UIGridLayout`, …) and constraints.

**What game creators can do:**
- Reduce the number of on-screen GUI objects; hide or remove UI that isn't currently visible.
- Flatten layout nesting — avoid a layout inside another layout where a single one would do.
- Use `AutomaticSize` only where it's actually needed (it adds measurement passes).
- Batch UI property changes into one frame instead of spreading them across frames — each change re-runs layout.
- Keep style rules and selectors simple (avoid deep descendant selectors).


---

### Pass2d / Pass3dAdorn

**Renders 2D GUI passes and 3D adornments (BillboardGuis, selection boxes, etc.).**

**Pass3dAdorn** renders 3D adornments — BillboardGuis and other labels above objects (Humanoid name/health labels), selection boxes, and debug adorns. For Humanoid labels that require line-of-sight, it raycasts to check whether the label is occluded. **Pass2d** readies 2D UI rendering (both player and Roblox UI).

**What game creators can do:**
- Reduce the number of visible adorned objects (BillboardGuis, Humanoid name/health labels)
- Reduce the number of visible parts
- Reduce the amount or complexity of UI elements


---

### AlwaysOnTopAdorns / CoreGuiAdorns / DebugAdorns / DebugAndEditAdorns

**Renders special adorn categories: always-on-top, core UI, debug, and edit mode.**


---

## Frame Lifecycle

### FrameRateManager::submitCurrentFrame

**Submits frame timing data to the Frame Rate Manager for adaptive quality decisions.**


---

### Present

**Presents the rendered frame — swaps the back buffer to the display.**

The final step that makes the rendered frame visible. May block if VSync is enabled and the GPU isn't done.

**Performance notes:** If Present is consistently wide, the application is GPU-bound (waiting for the GPU to finish). If it's consistently very fast, the application is CPU-bound.

**What game creators can do:**
- Reduce scene complexity; if Present is long, you may be GPU-limited


---

### waitUntilCompleted

**Waits for the GPU to finish rendering the previous frame.**

A sync point where the CPU waits for the GPU to complete the previous frame's rendering.

**Performance notes:** If this scope is wide, the application is GPU-bound — the GPU has not finished the previous frame's work.

**What game creators can do:** This is a *symptom*, not a cost in itself — the CPU is idle waiting for the GPU. Reducing CPU/script work won't help; reduce GPU load instead (see the GPU-group `Scene` scope): fewer transparent objects and less overdraw, simpler materials, and fewer/cheaper post-effects.


---

## Context / Low-Level

### Context::beginFrame / Context::endFrame() / Context::copyFramebuffer / Context::generateMipMaps / Context::resolveFramebuffer

**Low-level graphics context operations — frame boundaries and resource management.**


---

### uploadBufferData

**Uploads vertex/index/constant buffer data from CPU to GPU.**

**Performance notes:** Cost depends on data volume. High when many meshes change (EditableMesh, particles).


---

### RTPool::getRT

**Creates a new render target when the pool has no reusable match — used for off-screen rendering.**


---

### Copy / Copy Data / CopyTexture::LightMap / CopyTexture::Skylight

**GPU resource copy operations.**


---

## Highlights

### renderHighlightIdPass / renderMobileHighlightDepthMarkPass / renderMobileHighlightIdsPass

**Highlight system passes — renders object IDs for the Highlight effect.**


---

## Miscellaneous

### renderPerformanceOverlay

**Renders the performance debug overlay (light counts, draw call counts, etc.).**


---

### SortQueueJob

**Sorts the render queue on a worker thread.**


---

### meshTaskJobManager

**Terrain mesh generation job manager — generates terrain render meshes in the background.**


---

### TextureCompositorJob::update

**Updates the texture compositor — checks that all layer assets (meshes/textures) for avatar layered textures have loaded and updates their request priorities.**


---

### UITextureRenderer::renderJob / UITextureRendererBaseline::renderJob / UITextureJob::updateAndGetFramebuffer

**UI texture rendering jobs — renders off-screen UI textures (CanvasGroups, ViewportFrames).**


---

### EnvMapView::cullJob / EnvMapRaycastContext::raycastProcessingTask

**Environment map culling and raycast processing for reflections.**


---

### EditableImage::drawImageProjected / EditableMesh::updateRenderMesh / EditableMeshData::convertFileMeshDataToEditableMeshData / EditableMeshUpdateQueue::flushUpdates

**EditableImage and EditableMesh rendering updates — processes changes to editable assets.**

**Performance notes:** Frequent EditableMesh/EditableImage updates are expensive.

**What game creators can do:**
- Batch EditableMesh modifications
- Minimize per-frame pixel writes to EditableImage
- Use lower-resolution EditableImages


---

### FACSRigCFrameProvider::prepare / FACSRigCFrameProvider::update

**FACS (Facial Action Coding System) rig updates — face tracking data to bone transforms.**


---

### updateAvatarMemoryTracking / updateAvatarMemoryTrackingSummary

**Tracks avatar rendering memory usage for budget management.**


---

### updateEffectBoundings

**Updates bounding volumes for trail effects.**


---

### LightGrid::createCPU / LightGridCPU:: (variants)

**CPU-side light grid operations — computes lighting for the voxel-based light grid.**


---

### DM

**Syncs VR device state into the DataModel (VRService) — VREnabled and head/floor/hand UserCFrame poses each frame while VR is active.**


---

## Scene Update Details

### (Lookup)Build list of new tiles

**Diffs currently loaded virtual texture tiles against requested tiles and returns the list of new tiles to load.**


---

### BuildDeformedMeshPart

**Builds deformed mesh geometry for the wrap deformer system using target and cage vertices.**


---

### Dedup sampled chunks

**Deduplicates sampled terrain chunks to avoid redundant processing.**


---

### DrawBlockFilter

**Filters visible opaque blocks by render queue ID — removes already-rendered handles.**


---

### DynamicGeometryManager::prepareMeshUpdates::perEditableMesh

**Processes per-editable-mesh binding updates for all consumers.**


---

### DynamicGeometryManager::reallocateEditableMeshBinding

**Reallocates GPU buffers for editable mesh vertices and indices when size changes.**


---

### DynamicGeometryManager::transcodeBoxMappedVerticesAndCalculateBounds

**Transcodes box-mapped vertices from editable mesh format and calculates bounding volumes.**


---

### DynamicGeometryManager::transcodeFaceFilteredVerticesAndCalculateBounds

**Transcodes face-filtered vertices from editable mesh data and calculates bounds.**


---

### EditableMeshUpdateQueue::flushUpdates::perEditableMesh

**Updates the render mesh for each queued editable mesh modification.**


---

### EnvMapSampleRaycastStep

**Performs indoor raycasting for environment map sampling — determines room boundaries.**


---

### EnvMapUpdateStep

**Updates environment map with filtered raycast results to determine indoor/outdoor candidates.**


---

### FastCluster::finalizeSkinningAndEntities

**Clears the cluster's existing entities before new avatar-cluster geometry is generated.**


---

### FastCluster::finishUpdatingGeometry

**Completes geometry update and records telemetry for avatar loading state.**


---

### FastCluster::updateEntity

**Updates entity data, handles invalidations and failsafe avatar fallback.**


---

### FastCluster::updateGeometry

**Updates geometry for fast cluster avatars including skinning and asset loading.**

**Performance notes:** Rebuilds when an avatar's parts, meshes, or skeleton change; cost scales with the number of parts on the avatar and is re-triggered by runtime mesh changes, rescaling, and active layered-clothing deformation.

**What game creators can do:**
- Keep avatar/model part and accessory counts reasonable.
- Avoid changing avatar meshes or rescaling avatars at runtime — each forces a geometry rebuild; set them up once.
- Limit the number of layered-clothing items, which add deformation work to each rebuild.


---

### FastCluster::updateInstanceData

**Updates per-instance data (transforms, colors) for all fast cluster entities.**


---

### FillSubmitThreadData

**Fills submission thread data for multi-threaded GPU command submission.**


---

### Generate tile update commands

**Generates tile update commands from changed terrain tiles for virtual texturing.**


---

### GeometryGenerator::fetchResources

**Fetches geometry resources — loads file meshes and handles asset loading for parts.**


---

### GetBestTechniques

**Retrieves best rendering techniques/shaders for materials per pass based on capability flags.**


---

### HumanoidAdornRaycasts

**Performs raycasts to determine humanoid name/health bar visibility.**


---

### HumanoidHealth

**Renders the humanoid health bar GUI adornment above characters.**


---

### HumanoidName

**Renders the humanoid name plate GUI adornment above characters.**


---

### Image::scale

**Scales an image texture by resampling all mip levels to new dimensions.**


---

### InsertRenderQueue

**Inserts items from the render queue into the dispatch scene view for submission.**


---

### LightExtentsQuery

**Queries lights within spatial extents using the light octree spatial structure.**


---

### LightFrustumQuery

**Queries lights visible within a frustum using the light octree spatial structure.**


---

### LightGridCPU::updatePrepare

**Prepares CPU light grid updates each frame — activates lighting for global shadows, refreshes lighting technology/brightness settings, and selects which dirty chunks to update within the per-frame budget.**


---

### Octree::maintenance

**Performs spatial structure maintenance — validates nodes and rebalances the spatial tree.**


---

### RenderWorkspaceAdorns

**Renders 3D adornments for workspace content (selection boxes, handles, etc.).**


---

### StandardAdorns

**Renders standard adornments including humanoid name cache raycasts.**


---

### PlayerAdorns

**Renders 3D adornments for player GUI content.**


---

### SceneUpdater::checkFastClusters

**Checks fast clusters for geometry changes that require visual updates.**


---

### SceneUpdater::processPendingAttachments

**Processes newly added Attachment instances for rendering (beam/constraint anchors).**


---

### SceneUpdater::processPendingMegaClusters

**Processes newly added terrain chunks for rendering.**


---

### SceneUpdater::processPendingParts

**Processes newly added parts into instanced rendering clusters.**


---

### SceneUpdater::updateModelClusters

**Updates model-level clusters and removes empty ones.**


---

### WaitForRenderThread

**Waits for the render thread to complete frame rendering before proceeding.**

**Performance notes:** If wide, the render thread is still busy with GPU work from the current/previous frame.


---

### TransferControlToRenderThread

**Submits rendering work to the dedicated render thread for processing.**


---

### updateInvalidParts / updateWaitingParts

**Processes parts that were invalidated or are waiting for resource loading.**


---

### updateLightingAsyncTask

**Sequential per-chunk light-grid propagation on the calling thread, after the parallel occupancy pass has finished.**


---

### updateTerrainPrepareChangedChunks

**Identifies and prepares terrain chunks that changed since last frame for mesh regeneration.**


---

### updateUsedChunkMaterials

**Updates the set of materials used by visible terrain chunks.**


---

### updateWater / updateWaterMaterialConstants / updateWaterMaterialParams

**Updates water surface rendering state and shader parameters, plus terrain material constants (tiling/atlas/blend/emissive) for all terrain materials.**


---

### processGizmos

**Processes constraint and attachment gizmos (hover/selection state and detail display).**


---

### queryExtents / queryFrustum / queryFrustumOrdered / queryOcclusion

**Spatial queries: extent-based, frustum-based (ordered and unordered), and occlusion queries.**

**queryFrustumOrdered** applies frustum culling so objects not visible are not rendered.

**What game creators can do:**
- High cost means lots of elements — use larger meshes with more detail rather than many small pieces


---

### relocateGrid

**Relocates the light grid when the camera moves to a new region.**


---

### smoothClusterGetOrCreateChunk / smoothClusterMarkDirty

**Terrain rendering chunk management — creates chunks or marks them for regeneration.**


---

### Draw Tiles

**Draws virtual texture tile content into a staging texture (later copied into the atlas).**


---

### Edge Expansion

**Renders tile border expansion using radial shader to expand edges of virtual texture tiles.**


---

### Emitter estimated boundings

**Estimates bounding volumes for particle emitters for culling purposes.**

**Performance notes:** Recomputed for a particle emitter whenever its properties change; cost scales with the number of active particle emitters being (re)estimated in a frame.

**What game creators can do:**
- Reduce the number of active `ParticleEmitter`s; pool and reuse them instead of constantly creating new ones.
- Batch emitter property changes rather than changing them every frame — each change re-triggers a bounds estimate.
- Avoid continuously varying an emitter's motion properties (speed, acceleration, spread), which keep re-triggering estimation.


---

### FXCompositing

**Composites glow and sun rays effects: binds source, glow, and sun rays textures and renders a fullscreen compositing pass.**


---

### Generate Mips Custom

**Generates custom mipmap levels for font atlas textures through downsampling and copying passes.**


---

### HbaoRenderCompute

**Computes HBAO (Screen-space ambient occlusion) using compute shaders.**


---

### IndoorSkybox

**Renders a fallback indoor environment cubemap (per cube face) used as a reflection/image-based-lighting source for indoor areas — not a visible sky replacement.**


---

### InstanceGlob

**Performs GPU buffer uploads of instance data, handling both stable-slot path uploads and overflow entries via compute shaders.**


---

### LightGridCPU::LightGridCPU / LightGridCPU::updatePerform / LightGridCPU::updateChunksAsyncTask / LightGridCPU::updateChunkOccupancy

**CPU light grid lifecycle and update phases: construction, per-frame update, async chunk updates, and occupancy tracking.**

Updates the voxel lighting, which is used at lower quality levels.

**What game creators can do:**
- Reduce chunk occupancy (the amount of geometry occupying light-grid chunks)
- Use fewer lights
- Prefer non-shadow-casting geometry where shadows aren't needed


---

### LoadImage

**Loads and decodes an image file into a GPU texture.**

**What game creators can do:**
- Reduce the use of large images


---

### MaterialGenerator::garbageCollectIncremental

**Incrementally garbage-collects unused material GPU resources.**


---

### MeshReceived

**Processes a mesh asset that finished downloading — decodes and prepares for rendering.**


---

### MotionBuffer / MotionBuffer Fast Cluster Dispatch / MotionBuffer Instanced Cluster Dispatch / MotionBuffer update fast cluster movement / MotionBuffer update non fast cluster movement

**Motion vector generation for temporal effects — computes per-pixel motion for TAA, motion blur, and reprojection. Separate passes for fast clusters and instanced clusters.**


---

### OpenXrBeginFrame / OpenXrEndFrame / UpdateOpenXr

**OpenXR VR frame lifecycle — begins/ends the VR frame and updates tracking state.**


---

### Outline Effect

**Renders outline effects (Highlight instance outlines).**


---

### Parallel FastClusters

**Updates fast cluster rendering data in parallel across worker threads.**


---

### Parse and Layout RichText

**Parses rich text markup and computes layout for styled text content.**


---

### ParticleLighting

**Renders particle lighting via a lighting atlas: clears the atlas framebuffer, binds lighting resources, and draws particles as triangles to generate per-particle lighting.**


---

### Prepare2dWindow / PrepareBound / PrepareStandalone

**Prepares different window/surface types for rendering (2D window, bound surface, standalone).**


---

### ProcessUpdaterQueue

**Processes queued scene updater operations.**


---

### Processes Feedback / Processes Feedback (bypass)

**Processes virtual texture feedback requests — determines which tiles need loading. Bypass path skips when no feedback is available.**


---

### Profiler / ProfilerAdorn

**Renders the profiler UI and profiler adornments.**


---

### RaycastIndoorSingleRay / RaycastPointsClear

**Indoor environment raycasting for the PBR reflection probe: sampling rays to determine room boundaries, plus line-of-sight checks between two points.**


---

### Render feedback / RenderTiles

**Renders virtual texture feedback and tile content.**


---

### RenderView

**Renders one complete view (the main camera view or a secondary view).**


---

### Reproject

**Reprojects previous frame cloud data for temporal coherence: swaps cloud framebuffers and renders a reproject + filter pass.**


---

### SSAO / SSAOApply

**Screen-space ambient occlusion computation and application to the scene.**

**What game creators can do:**
- Reduce the number of post-processing effects; usually not significant


---


### SceneUpdater::invalidatePartMaterial

**Marks parts with material changes for shader/batch reassignment.**


---

### SceneUpdater::preparePerformUpdateCoordinateFrame

**Updates coordinate frame matrices for part transformation data.**


---

### ShadowMapSystem::updatePerform

**Performs shadow map rendering — the GPU-side shadow pass execution.**

Updates shadow maps. Skipped or reduced at lower graphics quality levels.

**What game creators can do:**
- Reduce the number of lights
- Use `Light.Shadows` and `BasePart.CastShadow` to disable shadow casting on less important instances


---

### Sort active chunks

**Sorts active terrain chunks by priority for rendering/streaming.**


---

### SunRaysBlur / SunRaysCompute / SunRaysDownSample

**Sun ray effect sub-passes: computes light shafts, blurs them, and downsamples for efficiency.**


---

### SwOcc main loop / SwOcc setup

**Software occlusion culling — rasterizes simplified occluders on the CPU to cull hidden objects before GPU submission.**

These are the top-level scopes for the software occlusion system. `SwOcc main loop` is the per-frame orchestrator and `SwOcc setup` prepares candidate occluders.

**Performance notes:** Runs on CPU threads and benefits scenes with high depth complexity. If these scopes are collectively wide, the scene has many potential occluders/occludees.


---

### Synthetic feedback

**Generates synthetic virtual texture feedback for tiles that weren't rendered but are needed.**


---

### Text Shaping+Layout / Typesetter::layout / Typesetter::shape / getHbFont / hb_shape

**Text rendering pipeline: font lookup, text shaping (glyph selection and positioning), and layout computation.**

**Performance notes:** Runs when text or its layout changes; cost scales with the amount of text shaped and with the number of separate font runs in a string (each distinct font is shaped separately).

**What game creators can do:**
- Reduce the total amount of on-screen text, and avoid re-setting `Text` every frame — each change re-shapes it.
- Update only what changes (e.g. a single number) rather than rewriting a whole label, so unchanged text isn't re-shaped.
- Minimize font changes within a single rich-text string; each distinct font becomes a separate shaping run.


---

### TextureAtlas::upload

**Uploads texture atlas data to the GPU.**


---

### TextureCompositor::render / TextureCompositor::update

**Composites layered textures (avatar clothing, decals) and updates compositor state.**


---

### TextureManager::loadImage / TextureManager::processTextureQualityChange

**Loads image textures and processes texture quality changes from the performance control system.**


---

### TreeTraversal

**Traverses terrain spatial structure for LOD chunk activation: processes destroy handles, updates chunks based on point of interest, and activates/deactivates chunks by camera distance.**


---

### UITextureRenderer::compositeUITextures / UITextureRendererBaseline::compositeUITextures / UITextureRendererBaseline::recurseDependencyTree

**Composites UI textures (CanvasGroups) and resolves their dependency trees.**


---

### UpSample / Upsample

**Upsamples a lower-resolution render target back up the mip chain: bloom (Glow) combines its blurred mip levels, and volumetric clouds upsample their half-resolution buffer.**


---

### UploadingMeshSizeBucketTelemetry / UploadingMeshTTMQTelemetry / UploadingMeshTimeToServeTelemetry / UploadingRenderFidelityTelemetry

**Telemetry scopes for mesh metrics: size distribution and time-to-serve of mesh uploads, plus render-fidelity (LOD/detail-level) analytics that are not upload measurements.**


---

### VertexStreamer::renderPrepare / WorldSpaceStreamer::renderPrepare

**Prepares streaming vertex data and world-space streamer for the render pass.**


---

### ViewportFrameSky

**Renders sky within ViewportFrame contexts.**


---

### VisibleQuery

**Performs a visibility query to determine which objects are in view.**


---

### VrSubmitFrame / vrPrepareFrame / waitVR / xrWaitFrame

**VR frame lifecycle — prepares, submits, and synchronizes VR frames.**


---

### WaitForLock

**Waits for a rendering lock to be released.**


---

### acquireFramebuffer / acquireSwapchains

**Acquires the framebuffer and swap chain images for the current frame.**


---

### add tile mips / applyMipResults / computeMipRequirements

**Virtual texture mipmap management — adds mip levels, applies results, and computes requirements.**


---

### asyncGrassTask / generateGrass / uploadGrass

**Grass rendering: generates grass geometry asynchronously and uploads to GPU.**


---

### blurRender

**Generic screen-space Gaussian blur pass used by post-processing effects (e.g. depth of field, bloom).**


---

### cameraOnHeartbeat

**Updates camera state on the heartbeat — drives constant-speed interpolation (Studio) and signals when a smooth transition is complete.**


---

### clear

**Clears invalidation event queue and merged invalidate events in dispatch scene.**


---

### computeUploadFull / computeUploadStaging / computeUploadStagingOverflow

**GPU compute upload passes — full upload, staging area, and overflow handling.**


---

### copyLockedContent

**Copy-on-write clone of voxelizer occupancy data when a write is needed while readers hold it.**


---

### copySkylight

**Copies combined skylight+sunlight data into a destination texture buffer for processing.**


---

### count covered pixels

**Counts how many feedback-buffer pixels each virtual texture tile covers, driving which terrain texture tiles to stream in.**


---

### destroyGeometry

**Destroys geometry resources that are no longer needed.**


---

### encodeDXT5 / encodeETC2

**Real-time texture compression — DXT5 (desktop) and ETC2 (mobile) formats.**


---

### endBatcher

**Completes instance handle batch upload and allocates buffer if needed.**


---

### filterRequests

**Filters pending terrain/mesh requests by priority and budget.**


---

### fitIntoMeshesBudget

**Manages mesh LOD promotion within video memory budget: assigns priorities, sorts, promotes LODs within budget, unloads lower priority, and applies requests.**


---

### gpuVoxelsPerform / gpuVoxelsPrepare / gpuVoxelsProcessChunkTableUpdates / gpuVoxelsProcessFeedback / gpuVoxelsProcessRequestedChunks / gpuVoxelsUpdateCompressedChunkPriorityQueue / gpuVoxelsUploadSubtree

**GPU voxel terrain system — prepares, processes feedback, updates chunk tables, handles requests, and uploads subtrees.**


---

### invalidateFailedResource

**Invalidates a resource that failed to load — marks it for retry or fallback.**


---

### lightingClearSkylightSunlightChunk / lightingComposit / lightingGetLights / lightingUpdateChunkGlobal / lightingUpdateChunkLocal / lightingUpdateChunkSkylight / lightingUploadChunk / lightingUploadCommit

**Light grid computation sub-phases: clears skylight data, composites lighting, computes shadow masks, gathers lights, updates chunks (local/global/skylight), updates individual light types, and uploads/commits results to GPU.**


---

### meshChunk / readChunk / readChunkBorder / readChunk_async / readTask

**Terrain mesh chunk operations — generates, reads, and processes terrain mesh chunks and their borders.**


---

### occupancyUpdateChunkPerform / occupancyUpdateChunkPrepare

**Updates light grid chunk occupancy — determines which grid cells contain geometry.**


---

### plantGrass

**Plants grass decoration geometry on terrain surfaces.**


---

### prepareProfiler

**Prepares profiler data for on-screen rendering.**


---

### processDestroyQueue

**Destroys terrain render chunks queued for teardown.**


---

### processTextureQuality

**Adjusts texture mip levels up or down each frame based on GPU memory pressure against configured budget thresholds.**


---

### queryFrustumEnvMap / queryResults / querySphere

**Reads back GPU timer-query results to compute the previous frame's GPU time.**


---

### queuePresent / submitAndFlip

**Queues the present operation and submits/flips the swap chain.**


---

### releaseResources

**Releases GPU resources that are no longer referenced.**


---

### remove overflow tiles

**Removes virtual texture tiles that exceeded the atlas capacity.**


---

### render2D / render3D

**VertexStreamer adorn rendering — render3D draws world-space adorn geometry; render2D draws screen-space adorn geometry via an orthographic camera.**


---

### requestMeshContent

**Applies pending mesh LOD requests within the memory budget — both unloading/evicting LODs and requesting new LOD downloads from the asset system.**


---

### resetCommandPool

**Resets the GPU command pool for reuse in the next frame.**


---

### sampleGpuCounters / sampleGpuCountersImplementation

**Samples GPU hardware performance counters.**


---

### sortUpdates

**Sorts pending updates by priority before processing.**


---

### synthesize tile requests

**Synthesizes virtual texture tile requests from visible renderables — the CPU fallback path used when the GPU feedback buffer is not consumed.**


---

### trimMemory

**Trims GPU memory by evicting low-priority resources.**


---

### updateChunk / updateChunkSkirtMasks

**Updates terrain rendering chunks and their skirt masks (LOD seam hiding).**


---

### updateDynamicObjectList

**Removes old dynamic object status after 32 frames by checking entry age.**


---

### updateLod / updateTransitionState

**Updates terrain LOD levels and transition blending state.**


---

### updateParticles / updateParticlesNonVisible / updateParticlesVisible / updateParticleBoundings

**Updates particle simulations — visible particles, non-visible particles (for streaming back), and their bounding volumes.**


---

### updatePlaneEffect

**Updates plane-based visual effects.**


---

### updateTerrainPrepareChangedChunksFilter

**Sorts the changed-terrain-chunk list by LOD and coordinate, then deduplicates it before preparation.**


---

### updateVR

**Updates VR-specific rendering state (head tracking, eye buffers).**


---

### uploadChunk

**Uploads a single terrain chunk's mesh data to the GPU (vertex/index buffers). Queue iteration is handled by the caller.**


---

### videoPresent

**Presents a video frame to the display.**


---

### waitOnGpu

**Synchronization points — waits for async worker thread or GPU completion.**

**Performance notes:** A wide wait means the CPU is idle waiting for something else to finish — most often the GPU (the frame is GPU-bound), sometimes a worker thread.

**What game creators can do:** This is a *symptom* of being GPU-bound, not a cost to optimize directly. Reduce GPU load (see the GPU-group `Scene` scope): fewer transparent objects and less overdraw, simpler materials, and fewer post-effects.


<br>
<br>

---

<a name="group-gpu"></a>
# 📄 `gpu.md` — GPU Group

<sub>[↑ Contents](#contents)</sub>

All scopes in this file are **hardware GPU-timed** — they measure actual GPU execution time using hardware queries, not CPU submission time. They appear on the GPU timeline in the profiler.

Many GPU scopes have a corresponding "Render" group scope that measures the CPU-side cost of submitting the same work. When you see both in a capture, the Render scope shows how long the CPU spent preparing/submitting commands, and the GPU scope shows how long the GPU spent executing them.

---

## Scene Rendering

### Scene

**The main GPU scene render — draws the 3D scene (geometry with full shading), typically the dominant GPU cost in a frame.** The same `Scene` scope name is also emitted for the 2D screen-space pass (BillboardGuis and screen-space adornments) and, in VR, for each eye's scene render. (The CPU cost of submitting this pass is the Render-group `Scene` scope.)

**Performance notes:** GPU vertex- and pixel-shading plus fill cost. Driven mainly by:
- Shader/material complexity — PBR materials and SurfaceAppearance cost more per pixel
- Overdraw — transparent objects and layered decals shade the same pixels repeatedly and prevent early-Z rejection
- Render resolution and how much of the screen shaded geometry covers

**What game creators can do:**
- Minimize transparent objects and overlapping/layered transparency (the biggest overdraw source)
- Use simpler materials and lower detail for distant objects
- Prefer simpler materials where full PBR / SurfaceAppearance detail isn't needed
- Keep visible triangle counts reasonable for objects that cover a large part of the screen

---

### Scene2D

**2D screen-space rendering on the GPU — BillboardGuis, particles in screen space.**

---

### GuiScene

**ViewportFrame GPU rendering — each ViewportFrame gets its own GPU render pass.**

---

### UI

**GPU rendering of all 2D user interface elements.**

---

### Window

**GPU rendering of the application window content.**

---

## Depth Passes

### DepthPrePass

**GPU depth-only rendering — populates the Z-buffer for early rejection.**

---

### Depth / DepthMip

**GPU depth downsampling for SSAO — halves depth resolution before ambient-occlusion computation.**

---

### OpaqueWithAlphaDepthPass / OpaqueWithAlphaLateDepthPass

**GPU depth passes for alpha-tested materials (foliage, fences) — early and late variants.**

---

## Shadows

### ShadowMap / Shadows / Shadow Blur

**GPU shadow map rendering and filtering.**

---

### StencilMark

**GPU stencil marking for shadow map regions.**

---

### EVSM Scroll (Copy) / EVSM Scroll (Remap) / EVSM VBlur

**GPU shadow atlas management — scrolling, remapping, and vertical blur for soft shadows.**

---

### Convert into EVSM and HBlur

**GPU conversion of depth maps to shadow format with horizontal blur.**

---

### clearShadowAtlas / clearChart

**GPU clear operations for shadow atlas regions.**

---

### depthPass / depthCachePass / dispatchView / renderShadowQueue

**GPU shadow rendering sub-passes — depth rendering from the light's perspective (`depthPass`), cached depth reuse (`depthCachePass`), per-view dispatch (`dispatchView`), and submission of all shadow-caster geometry for one light (`renderShadowQueue`).**

---

## Post-Processing

### Glow / BlurFx / BlurX / BlurY / blurRender

**GPU bloom and blur effects — multi-pass multi-pass blur.**

---

### DOF / Depth of Field / Bokeh Blur / Optimized Bokeh Full Resolution / Resolve Depth of Field Full Resolution

**GPU depth-of-field computation — blur based on depth distance from focus plane. Bokeh variants for high-quality circular blur shapes.**

---

### SunRays / SunRaysBlur / SunRaysCompute / SunRaysDownSample

**GPU god ray effect — radial blur from sun position, with downsample and blur passes.**

---

### ColorCorrection / Image Composition / FXCompositing

**GPU post-process compositing — `ColorCorrection` and `Image Composition` apply tonemapping and color grading, while `FXCompositing` composites the glow and sun-rays buffers.**

---

### SSAO / SSAOApply / Hbao PS / HbaoRenderCompute / Blur & upsample AO

**GPU ambient occlusion — computation, application, and blur/upsample passes.**

---

### Eye

**GPU per-eye scene render in VR — times the full scene render pass for one stereo eye (multiview or split-stereo).**

---

### MotionBuffer / MotionBuffer Fast Clusters / MotionBuffer Instanced Clusters / MotionBufferASW

**GPU motion vector rendering for temporal effects — separate passes for fast clusters and instanced clusters. ASW variant for VR reprojection.**

---

### Reproject / Reproject & filter

**GPU temporal reprojection — `Reproject` reprojects the previous frame's volumetric cloud data, while `Reproject & filter` reprojects and filters the previous frame's ambient-occlusion (SSAO) result for temporal stability.**

---

### Accum

**GPU accumulation pass in clustered light culling — accumulates per-froxel light visibility into the light grid.**

---

## Lighting

### FroxelGrid

**GPU clustered light culling — assigns lights to froxel tiles for efficient forward rendering.**

---

### ParticleLighting

**GPU particle lighting via a lighting atlas — draws particles as triangles to compute per-particle light contribution.**

---


## Sky & Environment

### AdvSky / Sky / Clouds / CloudsComp

**GPU atmospheric sky rendering and volumetric cloud computation.**

---

### EnvCapture / EnvMap / Generate Reflection / PrefilterSpecular

**GPU environment map capture (`EnvCapture`/`EnvMap`) and specular prefiltering (`PrefilterSpecular`); `Generate Reflection` is separate and generates the volumetric-clouds reflection texture.**

---

### IndoorSkybox

**GPU indoor skybox rendering — renders environment map faces converting sRGB to linear.**

---

### ViewportFrameSky

**GPU sky rendering within ViewportFrame contexts.**

---

## Terrain & Virtual Texturing

### TerrainFeedbackJittered / TerrainFeedbackTiled

**GPU virtual texture feedback — determines which texture tiles are needed at what resolution.**

---

### Draw Tiles / RenderTiles / Compress Tile

**GPU virtual texture tile management — renders and compresses the texture tiles for the material/terrain virtual texture.**

---

### Edge Expansion

**GPU virtual texture tile border expansion — expands tile edges to prevent seam artifacts between tiles.**

---

## Compute & Post-Processing Passes

### Compute

**GPU screen-space ambient occlusion (SSAO) buffer computation pass.**

---

### computeUploadFull / computeUploadStaging / computeUploadStagingOverflow

**GPU upload of per-instance transform/object data into the instance buffer — a full re-upload path, an incremental staging path, and a staging-overflow path.**

---

### Average tiles / Spread tiles / Spread vertical / Edge Fatten

**GPU Depth-of-Field near-field passes — average and spread the Circle-of-Confusion map, and fatten edges to hide disocclusion artifacts. (Tile here refers to DOF work tiles, not virtual-texture tiles.)**

---

### Count offsets

**GPU clustered-lighting step — counts the per-froxel light-list offsets while assigning lights to froxels (part of froxel light culling).**

---

## Resolution & Depth Management

### DownSample / UpSample / Upsample

**Shared post-processing downsample/upsample passes.** `DownSample`/`UpSample` are reused across the bloom/glow pipeline: `DownSample` builds the glow mip chain and also produces reduced-resolution buffers in the post-FX composite/tonemap stage; `UpSample` recombines the glow mip chain. The separate lowercase `Upsample` is the volumetric-clouds half-resolution upscale. They are not a single subsystem — the same scope names are emitted from different post-process passes.

---

### Upscale SmootherStep / Upscale Lanczos / Upscale Linear

**GPU upscaling algorithms for final image output.**

---

### Linearize Depth to color target / Linearize full resolution depth / Linearize half resolution depth

**GPU conversion of non-linear Z-buffer to linear depth for effects that need it.**

---

### Capture Depth to R32F

**GPU depth buffer capture into R32F format texture.**

---

### Downsample & mask 1/4th or 1/2  / Downsample & mask 1/4th or 1/2 width

**GPU depth-of-field preparation — downsamples and masks at quarter/half resolution.**

---

### Extract Alpha and spread width wise

**GPU DOF alpha extraction and horizontal spreading.**

---

### Copy and Compute CoC Full Resolution

**GPU computes Circle of Confusion at full resolution for depth-of-field.**

---

## Copy & Clear Operations

### Clear / ClearColor / ClearColorCmask / ClearDepthStencil / ClearTexture / Context::clearFramebuffer

**GPU clear operations: `ClearColor`/`ClearColorCmask` clear color render targets, `ClearDepthStencil` clears depth/stencil, `ClearTexture` clears textures, and `Context::clearFramebuffer` clears the framebuffer; `Clear` clears the clustered light-culling buffers via a compute pass.**

---

### Copy / Copy Color / Copy Color Full Resolution / Copy from Scratch

**GPU resource copy operations between render targets.**

---

### CopyTexture::LightMap / CopyTexture::Skylight

**GPU copies of light map and skylight texture data.**

**Performance notes:** Fires when lighting/skylight data is recomputed and re-uploaded; cost scales with how much of the light data changed — driven by dynamic lighting and skylight / time-of-day changes.

**What game creators can do:**
- Keep lighting static where possible — moving, adding, or removing lights forces light-texture re-uploads.
- Avoid rapid or continuous skylight / time-of-day changes; step them less often rather than every frame.

---

### ResolveColor / msaaResolveFrameBuffer / Software Depth Resolve

**GPU MSAA resolve — converts multi-sample targets to single-sample. Software depth resolve for platforms without hardware resolve.**

---

## Normal & Spatial

### Gen Normals / Sample World Pos

**`Gen Normals` generates normals for screen-space effects; `Sample World Pos` is a debug tool that samples the depth buffer to compute a single world-space point on demand (not a per-frame pass).**

---

### Dilate Temporal

**GPU SSAO temporal-stability pass — spreads less-stable areas of the ambient-occlusion result so moving objects get fresh AO values.**

---

### Gather

**GPU light-culling gather pass — assigns visible lights into the clustered light-grid tiles.**

---

## Instanced Rendering

### InstanceGlob

**GPU buffer uploads of instance data — handles stable-slot uploads and overflow entries via compute shaders.**

---

### TextureCompositor::render

**GPU texture compositing — layers avatar clothing and decal textures.**

---

### gpuCompress

**GPU real-time texture compression for dynamically generated textures.**

---

### Generate Mips Custom

**GPU custom mipmap generation for font atlas textures through downsampling passes.**

---

## Highlights

### Outline Effect

**GPU highlight outline computation with color LUT and MSAA handling.**

---

### renderHighlightIdPass / renderMobileHighlightDepthMarkPass / renderMobileHighlightIdsPass

**GPU highlight ID rendering for the Highlight instance effect (desktop and mobile variants).**

---

## Miscellaneous

### MSAA

**GPU MSAA resolve pass.**

---

### AlwaysOnTopAdorns

**GPU rendering of always-on-top adorns into the resolved color buffer, after bloom sampling but before image composition.**

---

### PerformanceOverlay

**GPU performance debug overlay rendering.**

---

### WaitUntilSafeForRendering

**GPU fence wait — ensures previous frame work is complete before starting new frame.**

---

### Dispatch / Cull / CullCPU

**GPU command dispatch, GPU-side culling, and CPU-side culling fallback.**

---

### UITextureRenderer::renderJob / UITextureRendererBaseline::renderJob

**GPU off-screen UI texture rendering for CanvasGroups.**

---

### Capture Depth to R32F / Context::clearFramebuffer

**GPU operations: captures the depth buffer to R32F format and clears the framebuffer via the graphics context.**


<br>
<br>

---

<a name="group-shadows"></a>
# 📄 `shadows.md` — Shadows Group

<sub>[↑ Contents](#contents)</sub>

The Shadows group contains scopes for the shadow mapping system: frustum computation, caster gathering, culling, atlas management, and rendering. Shadow work runs on the main thread (preparation) and worker threads (culling, rendering).

---

## System-Level

### ShadowMapSystem::updatePrepare

**Top-level shadow system update — prepares all shadow maps for the current frame.**

Updates the frame's shadow distance parameters and the sun and terrain light descriptors (direction, distance, softness). Atlas allocation, frustum building, culling, and rendering happen in separate scopes later in the frame.

**Performance notes:** If consistently wide, the scene has too many shadow-casting lights or too many shadow casters. Parts cast shadows regardless of transparency, under the assumption they may contain decals. Shadow recalculation is reduced or skipped at lower graphics quality levels.

**What game creators can do:**
- Reduce the number of lights with `Shadows = true`
- Increase `Lighting.ShadowSoftness` to reduce resolution needs
- Reduce `Lighting.ShadowDistance` to limit shadow range
- Use fewer shadow-casting PointLights/SpotLights (they each need 6/1 shadow maps)


---

### ShadowMapSystemNew

**Alternate shadow map system entry point.**


---

## Shadow Object Types

### ShadowObjectDirectional / ShadowObjectDirectionalNew

**Updates the directional light (sun) shadow cascades.**

Computes cascade frustums and updates shadow maps for the sun. Multiple cascades provide detail close to the camera and coverage far away.

**Performance notes:** Directional shadows are the most common and often the most expensive shadow type. Cost depends on cascade count, resolution, and caster count.


---

### ShadowObjectPoint / ShadowObjectPointNew

**Updates a point light shadow map (6-face cube map).**

Point lights require rendering the scene 6 times (one per cube face) for omnidirectional shadows.

**Performance notes:** Very expensive — 6× the cost of a single shadow map. Limit point light shadows.

**What game creators can do:**
- Use SpotLights instead of PointLights where possible (1 map vs 6)
- Reduce point light shadow range
- Limit the number of shadow-casting PointLights to 1-2


---

### ShadowObjectSpot / ShadowObjectSpotNew

**Updates a spot light shadow map (single frustum).**

Spot lights only need one shadow map (a single frustum projection).

**Performance notes:** Cheapest local light shadow type. One frustum, one render pass.


---

## Frustum & Cascade Management

### buildFrustums / TerrainShadow::updateFrustums / Merge frustums

**Computes and merges shadow frustums for cascade shadow maps.**

Determines the view volume each cascade covers, accounting for camera position and shadow distance.


---

### allocateAndBuildViewports / allocateDispatchViewsSpawnCullJobs

**Allocates atlas viewport space for each shadow map and spawns parallel culling jobs.**


---

### invalidatePerformCascades / invalidatePerformLights

**Marks shadow data as invalid when lights or geometry change, triggering re-render.**


---

### tiledScrollAtlasChart

**Scrolls the shadow atlas when the camera moves — reuses existing shadow data where possible.**

The shadow atlas uses tiled scrolling to avoid re-rendering the entire shadow map when the camera moves slightly.

**Performance notes:** Efficient reuse mechanism. Only new regions need rendering.


---

## Caster Gathering

### Get shadow casters / getPotentialShadowCasters

**Gathers all objects that cast shadows within the shadow frustum.**

Queries the scene's spatial structures to find objects that are within shadow-casting range.

**Performance notes:** O(objects in range). Large shadow distances with many objects increase cost.


---

### Filter cascade casters / Sort cascade casters / Fill casters / Fill tiled casters

**Filters, sorts, and fills the caster list for each cascade — determines which objects to render into each cascade's shadow map.**


---

### buildVisibleShadowObjectsList

**Builds the list of shadow-casting lights that are actually visible, then priority-sorts them (nested 'Priority sort' scope) so nearer/larger lights are updated first.**


---

## Culling

### Perform culling / Precise culling / Spatial culling

**Culls shadow casters against the shadow frustum — determines which actually contribute to shadows.**

Multiple culling phases: broad spatial culling followed by precise per-object culling.


---

### ShadowCullJob

**Worker thread shadow culling job — tests objects against shadow frustums in parallel.**

**Performance notes:** Runs on worker threads. Cost proportional to caster count.


---

### TileClassification

**Computes, per shadow caster, which atlas tiles the caster overlaps (tile coverage bitmasks). The re-render-vs-reuse decision happens later in 'Fill tiled casters'.**


---

## Rendering

### depthPass / depthCachePass

**Renders objects into the shadow map depth buffer.**

The actual shadow rendering pass — draws geometry from the light's perspective to generate depth.

**Performance notes:** Cost proportional to caster triangle count × shadow resolution.


---

### dispatchView

**Dispatches one shadow view's render commands to the GPU.**


---

### renderShadowQueue

**Submits the shadow render queue to the GPU — all shadow draw calls for one light.**


---

### updateFrustumRenderQueue

**Updates the render queue for a specific shadow frustum.**


---

## Blur & Filtering

### horizontalBlur / verticalBlur

**Shadow map blur passes — multi-pass blur for soft shadows.**

Shadows use multi-pass blur for smooth penumbra (shadow edge softness).


---

## Sorting & Priority

### Sort / Priority sort / Priority sort throttling

**`Sort` orders each shadow render-queue by material and index buffer to reduce GPU state changes (a draw-order sort). `Priority sort` orders shadow casters by distance/priority to decide which get updated, and `Priority sort throttling` caps how many are updated per frame.**

The shadow system can throttle lower-priority shadow maps (update them less frequently) to save performance.


---

## Legacy

### LegacyRenderNodes

**Prepares sun-shadow casters for the legacy (non-new-pipeline) code path — gathers and culls shadow casters against the cascade frustums and updates the per-cascade shadow frustums.**

Issues no draw calls; it collects visible casters, sorts them for stable timestamp hashing, computes cascade/frustum timestamps, and creates or reuses (caches) the cascade shadow frustums for later rendering.


---

### drawFrustums

**Allocates atlas viewports and builds the frustum update list, then dispatches the per-frustum shadow draw passes into the shadow atlas.**


<br>
<br>

---

<a name="group-script"></a>
# 📄 `script.md` — Script Group

<sub>[↑ Contents](#contents)</sub>

The Script group contains scopes for Lua/Luau script execution, parallel VM management, garbage collection, and deferred operations. These scopes fire on the main thread (serial execution) and worker threads (parallel Luau).

---

## Core Execution

### runSerial

**Resumes threads scheduled for serial execution during the Parallel Luau work-queue iteration — the serial half of the parallel/serial orchestration.**

During each Parallel Luau work-queue iteration this drains the serial-thread queue: threads that were scheduled to run serially (e.g. work deferred back from the parallel phase) are resumed one at a time. It is one part of the parallel/serial orchestration, not the main script-execution loop.

**Performance notes:** Wide `runSerial` means scripts are doing expensive work on the main thread. This directly blocks the frame.

**What game creators can do:**
- Move expensive computations to `task.desynchronize()` for parallel execution
- Break large loops with `task.wait()` to spread across frames
- Use native Luau types and avoid excessive table allocations
- Profile individual scripts using the ScriptProfiler


---

### runParallel

**Executes Lua scripts across parallel VM threads — distributes Actor-isolated scripts to worker threads.**

Scripts inside Actor instances that have called `task.desynchronize()` execute here in parallel across multiple worker threads. Each Actor's VM runs independently.

**Performance notes:** Wide `runParallel` on worker threads is generally fine (doesn't block the main thread). However, the main thread waits for all parallel work to complete before continuing.

**What game creators can do:**
- Balance work across Actors (avoid one Actor doing all the work)
- Keep parallel code free of calls that force synchronization
- Use SharedTable for data sharing instead of BindableEvents


---

### runSignals

**Delivers signal results from parallel execution back to the main thread.**

After parallel work completes, signals that were queued during parallel execution are delivered on the main thread. This includes BindableEvent signals and other cross-Actor communication.

**Performance notes:** Cost depends on the number of signals queued during parallel execution.


---

### resumeVMThreads

**Resumes Luau coroutines/threads that are ready to continue.**

Core VM thread resumption — wakes up coroutines that were yielded by task.wait(), events, or other yielding operations and have become ready.

**Performance notes:** Per-thread overhead is minimal. Total cost = number of threads being resumed × their execution time.


---

### resumeParallelWaitingScripts

**Orchestrates resumption of scripts waiting for parallel execution results.**

Coordinates the parallel → serial transition: collects parallel work, distributes to workers, waits for completion, then delivers results.

**Nesting:** Contains: collectParallel → runParallel → runSignals → runSerial.


---

### collectParallel

**Collects and buckets parallel-ready scripts into per-thread work queues.**

Organizes scripts that are ready for parallel execution into thread-safe buckets for distribution to worker threads.

**Performance notes:** Fast organizational pass. Cost proportional to the number of parallel-ready scripts.


---

### executeParallelHybridCallback

**Executes a hybrid parallel/serial callback — scripts that partially run in parallel.**

Used by the hybrid parallel queue where some work is parallel-safe but the callback needs both parallel and serial phases.


---

## Scheduling

### delayedThreads

**Resumes threads whose `task.wait(N)` or `task.delay(N)` timers have expired.**

Checks all sleeping threads and resumes any whose delay timer has elapsed. Runs at the start of each frame before other script work.

**Performance notes:** Cost proportional to the number of threads with expired timers. If many scripts use short delays (task.wait(0)), they all wake up every frame.

**What game creators can do:**
- Use longer delay values when possible
- Avoid task.wait(0) in tight loops (use task.defer instead if appropriate)
- Consolidate periodic logic into fewer scripts


---

### deferredThreads

**Processes callbacks queued with `task.defer()` — runs them at the end of the current resumption cycle.**

Executes all deferred callbacks that were queued during the current frame's script execution. Deferred callbacks run after the current batch of signal handlers completes.

**Performance notes:** Same as general script execution. Too many deferred callbacks can extend frame time.

**What game creators can do:**
- Reduce how much work you queue with `task.defer()` each frame; run work directly when deferral isn't needed.
- Avoid deferred callbacks that themselves queue more deferred work — this can snowball within a frame.
- Disconnect event connections you no longer need, and debounce or batch very high-frequency signal handlers.


---

### deleteDeferred

**Releases instances that were queued for deferred deletion, freeing them at a GC-safe point.**

Instances whose destruction must be delayed for safe Lua lifetime management are held in a pending free-list and released here; the list and each script VM's deferred-deletion list are cleared. This is unrelated to the deferred signal-behavior (event delivery) mode.

**Performance notes:** Typically fast. Can spike when destroying large instance hierarchies.


---

## Garbage Collection

### GC

**Runs one step of the Luau garbage collector — reclaims unused memory.**

Performs incremental garbage collection on the Luau heap. The GC runs in steps to avoid long pauses, but each step still takes time proportional to the amount of memory being scanned/collected.

**Performance notes:** GC cost is driven by:
- Total Luau heap size (more objects = more to scan)
- Allocation rate (more allocations = more frequent GC)
- Object connectivity (deeply linked tables are expensive to trace)

**What game creators can do:**
- Reduce table allocations in hot loops (reuse tables)
- Set unused references to nil so they can be collected
- Avoid creating closures in tight loops
- Use buffer/string.buffer for binary data instead of tables of numbers
- Profile memory with the Luau Heap Profiler


---

## Server Authority Script Execution

These scopes drive script execution under the **Server Authority** simulation system (opt-in Beta). Under Server Authority the client runs script logic ahead of server confirmation (client-side prediction) and re-runs it after a rollback when the server's authoritative state differs. `AuroraScript`/`AuroraScriptService` is the internal name for this system, so the scope strings carry that prefix.

### AuroraScriptService::onPreAnimation / onPreSimulation / onPostSimulation / onFixedHeartbeat / onPresentation

**Lifecycle hooks that drive Server Authority script execution at specific points in the frame.**

Each hook invokes scripts at the corresponding phase:
- `onPreAnimation` — before animation evaluation
- `onPreSimulation` — before physics
- `onPostSimulation` — after physics
- `onFixedHeartbeat` — fixed-rate heartbeat
- `onPresentation` — before rendering

**Performance notes:** Cost = script execution at that phase.


---

### AuroraScriptService::stepAllObjects

**Steps all Server-Authority-managed objects through one simulation frame.**


---

### AuroraScriptService::startAuroraServiceStep / completeAuroraServiceStep

**Bookends for a service step — initialization and completion phases.**


---

### AuroraScriptService::finishFrame

**Finalizes the frame — commits state changes and prepares for the next frame.**


---

### AuroraScriptService::iterativelyProcessMessages

**Processes inter-object messages iteratively until stable.**

Messages between objects can trigger cascading state changes. This scope iterates until no new messages are generated.

**Performance notes:** Normally fast. Can spike if objects create long message chains.


---

### AuroraScriptService::rollback / rollbackToWorldStep / rollbackEffectedObjectsAtFrame

**Reverts script state when the server's authoritative state diverges from the client's prediction.**

**Performance notes:** See Simulation group → `AuroraService::rollback` for full details. Frequent rollbacks usually indicate high latency or diverging client/server simulations.


---

### AuroraScriptService::snapshotRollback / simulateEngineHook

**Captures state snapshots for rollback support and hooks into the engine simulation.**


---

### AuroraScript::start / AuroraScript::stop

**Lifecycle management for individual scripts — initialization and teardown.**


---

### AuroraScript::invokeCallback

**Invokes a Lua callback function from the script system.**


---

### AuroraScript::processUpdate / processUpdates / processMessages

**Processes state updates and messages for one script.**


---

### AuroraScript::onStartFrame / onSendScheduledMessages / onSimulate / stepObjectsAtFrame / onFinishFrame

**Per-frame lifecycle hooks for individual scripts.**


---

### AuroraScript::readStateObject

**Reads a state object from the state store for script access.**


---

### AuroraScript::snapshotRollback

**Captures a rollback snapshot for one script.**


---

### AuroraScriptRuntime::invoke

**Low-level Luau callback invocation wrapper.**

**Labels:** Emits the function name being invoked — visible on hover in the timeline.


---

## CollectionService & Tags

These scopes fire when game code uses CollectionService (tagging system). They are typically called from Lua via `CollectionService:GetTagged()`, `:AddTag()`, etc.

### getCollection

**Retrieves the internal instance collection for a given class/type name — an engine-internal registry, distinct from the CollectionService tag query (`GetTagged`).**

**Performance notes:** Fast for small collections. Can be expensive if a tag has thousands of instances.


---

### addInstance / addTag / addTagHelper / addTagLua

**Adds a tag to an instance — registers it with CollectionService.**

**Performance notes:** Negligible per-call.


---

### removeTag / removeTagLua

**Removes a tag from an instance. (The `removeTagLua` label is reused by a few other CollectionService operations — reading an instance's tags (`GetTags`), listing every tag in use (`GetAllTags`), and instance teardown — so a `removeTagLua` block in a capture may be one of those rather than a tag removal.)**


---

### computeAddedAndRemoved

**Computes the tag diff for a single instance — which tag names were added or removed between its previous and current tag strings.**

Called from onTagsChanged when an instance's tags change, so that per-tag added/removed signals can be fired for that instance.

**Performance notes:** O(collection size). Fast for small collections.


---

### getTagged / getAllTags / getTagsLua

**Tag-query APIs backing the Lua `GetTagged` and `GetTags` calls (and the enumerate-all-tags query).**


---

### hasTag / hasTagLua

**Checks if an instance has a specific tag.**

**Performance notes:** O(n) in the number of tags on the instance — a linear scan of the instance's tag list, not a hash lookup.


---

### getInstanceAddedSignal / getInstanceRemovedSignal

**Returns the signal for tag add/remove events.**


---

### iterateCollection

**Iterates all instances in a collection, invoking a callback for each.**

**Performance notes:** Cost proportional to collection size.


---

### connectToSignal / connectPropertyChangedCallback / connectAttributeChangedCallback

**Connects Lua callbacks to instance signals within the collection system.**


---

### match / unmatch

**Matches or unmatches an instance against collection query criteria.**


---

### onTaggedInstanceServiceProvider / onTagsChanged / tagRemoved

**Internal event handlers for tag system state changes.**


---

### CollectionWatcherConstructor

**Constructs a CollectionWatcher — sets up monitoring for a tag collection.**


---

## Heap Profiling

These scopes fire when using the Luau Heap Profiler (ScriptProfiler). They are diagnostic and should not appear during normal gameplay.

### LuauHeapMemoryReport::runProcessAsync / runProcessImmediate

**Runs the heap memory report generation — either async (non-blocking) or immediate (blocking).**

**Performance notes:** Expensive — pauses Luau VMs and traverses the entire heap. Only used for diagnostics.


---

### LuauHeapMemoryReport::runGc / gc

**Forces a full GC cycle as part of heap profiling to get accurate live-object counts.**


---

### LuauHeapMemoryReport::collectInstances / collectBasicStats / prepareGraph / linkHeap / aggregate / collect / createResultTable / reportTelemetry / processCoroutineBody

**Sub-phases of heap analysis: collecting instance data, building the object graph, linking references, aggregating statistics, and generating the report.**


---

### LuauUniqueReferenceReportProcess:: (same sub-phases + walkRoots)

**Unique/leaked reference report — flags instances held by exactly one reference, and also instances not parented under the DataModel. Its sub-phases are collectInstances, prepareGraph, linkHeap, walkRoots, collectBasicStats, createResultTable, reportTelemetry, processCoroutineBody.**


---

### getLuauHeapMemoryReport / getLuauHeapInstanceReferenceReport / getLuauHeapJSONDataAsync / getRootObjectsMemoryAsync

**Top-level API entry points for heap profiling features.**


---

## Script Execution Scopes (Script Group)

### Script_*Name*

**Identifies which script is currently executing.**

When you see `Script_Animate`, `Script_CameraModule`, or any `Script_*` scope, it means that particular script's code is running. The name matches the script's `Name` property in the Explorer. The script can be yours or one of Roblox's internal or core scripts.

**Performance notes:** If a specific `Script_*` scope is wide, that script is consuming significant frame time. Open it in the Script Profiler for line-level detail.

---

### $ScriptOverflow / $ProfileBeginOverflow

**The profiler ran out of available scope slots for this category.**

When more unique script scopes are active than the pool can track, additional scopes are grouped into overflow buckets depending on their type:

- **`$ScriptOverflow`:** Used for scopes representing unique scripts. Viewers typically replace this technical name with the real script name stored in the scope's first label, prepended with an asterisk (e.g., `*Script_MyGameScript`).
- **`$ProfileBeginOverflow`:** Used for custom `debug.profilebegin` scopes. Viewers typically replace this technical name with the real scope name stored in the scope's first label, prepended with an asterisk (e.g., `*MyCustomLuauScope`).

**What game creators can do:**
- Reduce the number of `debug.profilebegin` regions in game scripts
- Avoid generating dynamic scope names
- Avoid having hundreds of single-function scripts

---

### ::*CoreScriptScope*

**A scope belonging to an internal or core script rather than game scripts.**

Scopes with names starting with `::` belong to scripts that are not directly included as part of your game (such as CoreScripts). The name of the script is always shown in its parent scope.

---

### debug.profilebegin / debug.profileend scopes

**Custom profiler scopes created by game scripts using `debug.profilebegin("name")` and `debug.profileend()`.**

These always appear nested inside the `Script_*` scope of the script that created them. Any scope name that doesn't match a known engine scope is likely a custom scope from game scripts. Developers use these to measure specific sections of their code.


<br>
<br>

---

<a name="group-network"></a>
# 📄 `network.md` — Network Group

<sub>[↑ Contents](#contents)</sub>

The Network group contains scopes for the replication system, instance streaming, join flow, packet processing, and bandwidth management. These scopes fire on the main thread (replication step) and on network worker threads (packet deserialization, HTTP).

---

## Replication Pipeline (MegaReplicator)

The MegaReplicator orchestrates all replication work each frame. These scopes run sequentially within the network step.

### Force World Assemble

**Forces all pending physics assemblies to be assembled before replication.**

Ensures the physics world is in a consistent state before replicating positions and velocities to clients.

**Performance notes:** Normally fast. Can spike after large batches of part additions.


---

### PrepopulateMRDs

**Pre-populates MegaReplicator Descriptors for new instances that need replication.**

Sets up the replication metadata for newly-created instances so they can be sent to clients.

**Performance notes:** Cost proportional to new instances per frame.


---

### RaiseCompleteness

**Propagates completeness state through the instance hierarchy — determines what clients can see.**

Completeness tracks which portions of the instance tree are fully replicated to a client. This scope propagates "complete" marks up the tree.

**Performance notes:** Can be expensive during initial join when large hierarchies become complete.

**Labels:** `"Completeness expected raised X/Y"` — shows how many instances had completeness raised vs total.

---

### Updating Mega World Extents

**Updates the spatial bounds of the replicated world for streaming calculations.**

Recalculates the bounding box of all streamable content to help the streaming solver determine what to send.


---

### Prepare Physics Rep Assembly List Sync

**Synchronizes the physics replication assembly list with the current world state.**

Ensures the list of physics assemblies that need replication matches what actually exists in the physics world.


---

### SharedQCleanUp

**Cleans up the shared replication queue — removes completed or expired items.**

Garbage-collects items from the shared data queue that have been sent to all relevant clients.

**Performance notes:** Normally fast. Can accumulate work if many items expire simultaneously.

**Labels:** `"SharedQueue: X (will clean up: Y)"` and `"Eligible Cleanup Items breakdown"` — shows queue size and cleanup volume.

---

### Deferred MotorAngles update

**Applies previously received, deferred Motor6D angle updates to the physics world (receive side).**

Runs in the physics receive job: iterates the deferred motor-angle entries and writes them onto each assembly's physics state. This is a receive-side application step, not a send-side batching step.


---

### GenerateJDIv2

**Generates Join Data Items v2 — prepares replication data for the join snapshot.**

Creates the data packets that form the initial state snapshot sent to newly-joining clients.

**Performance notes:** Expensive during player joins. Cost proportional to world size.


---

### Detect Join-Batchable Replicators

**Identifies replicators that can be batched together during join for efficiency.**

Groups related replication items that can be sent in a single batch rather than individually.


---

### Prebuild join cache

**Pre-builds cached data for the join snapshot to speed up subsequent joins.**

Caches serialized data that doesn't change between joins so it doesn't need re-serialization.


---

### ISR Update Timestamp Offsets

**Updates Instance State Replicator timestamp offsets for clock synchronization.**

Adjusts timestamps to account for clock differences between server and clients.


---

### Dispatch ISR Prep And Send

**Dispatches the ISR (Instance State Replicator) preparation and sending phase.**

The ISR handles fine-grained property replication. This scope prepares and sends property updates.

**Performance notes:** Cost proportional to the number of dirty properties this frame.


---

### Update and allocate bandwidth

**Allocates available network bandwidth to replicators based on priority.**

Distributes the frame's bandwidth budget across all pending replication items, prioritizing important data.


---

### Replicator Status Checks

**Checks replicator connection health — detects disconnects and timeout conditions.**


---

### Assign Network Object Priorities

**Assigns priorities to network objects based on distance, relevance, and importance.**

Determines which objects get bandwidth first based on their proximity to players, visibility, and game importance.

**Performance notes:** O(replicated objects). Can be expensive in large worlds.


---

### LR Pull Manager Notify / LR Pull Manager Generate

**Large Replicator pull manager — notifies and generates large data transfers.**

Handles the "pull" protocol for large objects that are requested by clients rather than pushed by the server.


---

### Dispatch Data Senders

**Dispatches all data sender tasks — sends queued replication data to clients.**

The main data sending phase. Iterates over all connected clients and sends their pending replication data. Sends property changes, remote event invocations, Humanoid state changes, animation state changes, and replication of new instances.

**Performance notes:** Cost = (connected players) × (data per player). The primary network bandwidth consumer.

**What game creators can do:**
- Reduce the number of replicated changes to the data model

**Labels:** `"Connected Players : X"` refers to the total number of players who finished joining the game, and `"Pending Players : X"` refers to the total number of players currently joining the game. These are frame-level Network labels emitted once per network step, not inside this scope.

---

### Send clusters

**Sends terrain data to clients.**

Serializes and sends terrain voxel data that clients need but don't have yet.

**Performance notes:** Expensive when players move into new terrain areas that haven't been streamed yet.

**What game creators can do:**
- Reduce the amount or size of terrain changes


---

### Cluster: insufficient buffer to send

**Diagnostic scope — fires when a cluster's data exceeds the send buffer.**

Indicates that terrain data is too large for the current buffer allocation.


---

### Dispatch PhysicsSenders and TouchSenders

**Sends physics state updates (positions, velocities) and touch events to clients.**

The physics replication sending phase. Sends CFrame/Velocity data for moving objects. This will soon be deprecated as Roblox transitions to use `ISRSendStep` to replicate physics data.

**Performance notes:** Cost proportional to moving physics objects × connected clients.

**What game creators can do:**
- Reduce the amount of moving objects and/or touches


---

### CompletenessPropagation / CompletenessFinalization / CompletenessUpdateExpectedChildren

**Multi-phase completeness state propagation and finalization.**

Propagates the completeness state through the instance tree and finalizes it for this frame.


---

### Preserialize network streams

**Pre-serializes network data streams — encodes data ahead of the send phase.**

Serializes replication data into the wire format before the actual network send. This allows serialization to happen separately from I/O.

**Performance notes:** CPU-bound serialization work. Cost proportional to data volume.

**Labels:** `"Items to preserialize : X"` — shows the work volume.

---

### Data Senders / Send Item

**Sends individual replication items to a specific client.**

Per-item, per-client send operation.


---

### Sending Globals / Sending Network Schema

**Two distinct join-flow scopes: `Sending Globals` sends global setup data to a newly-joining client; `Sending Network Schema` sends the serialization schema.**

Part of the join flow. `Sending Network Schema` transmits only the serialization schema (so the client can decode future messages), while `Sending Globals` transmits initial global setup data — the two are separate operations.


---

## Streaming System

### Dispatch StreamJob

**Dispatches the streaming job — determines what to stream in/out based on player position.**

The main streaming orchestrator. Calculates which instances should be streamed to or from each client based on their focus position and streaming radius.

**Performance notes:** Cost depends on world size and streaming density. Runs every frame on the server.

**What game creators can do:**
- Lower `Workspace.StreamingTargetRadius` — the main lever for streaming workload (`StreamingMinRadius` is the always-loaded floor that never streams out, so lowering it mainly reduces resident content rather than this dispatch cost)


---

### StreamJob

**The actual streaming computation — priority-based instance streaming decisions.**

Computes streaming priorities and makes send/remove decisions for each client's stream region.

**Performance notes:** Can be expensive in large worlds with many streamable instances.

**What game creators can do:**
- Set appropriate `StreamingMinRadius` and `StreamingTargetRadius` on Workspace
- Use `ModelStreamingMode` to control per-model streaming behavior
- Reduce the total number of instances in the workspace


---

### StreamingSolverV2

**The streaming solver — spatial computation for which instances are in range.**

Uses spatial data structures to efficiently determine which instances fall within each player's streaming radius.


---

### InstanceObjectManager

**Manages instance lifecycle for streaming — creates/destroys instances as they stream in/out.**

Handles the actual creation and teardown of instances on the client as they enter or leave the streaming radius.


---

### Replication Foci Streaming / Foci and Set Changes / Refresh Tracked Location

**Manages streaming focus points — where each client is "looking" for streaming purposes.**

Updates the spatial positions that define each client's area of interest.


---

### StreamingObserver processBatchedSetMoves / StreamingObserver sets change

**Processes batched set membership changes in the streaming observer.**

Handles instances moving between streaming sets (e.g., entering/leaving a client's radius).

**Performance notes:** Server-side. Cost scales with how many instances move across streaming regions and how often instances are created or destroyed.

**What game creators can do:**
- Reduce the number of parts that continuously move across large distances (fast projectiles, vehicles) — each region crossing is work here.
- Weld moving parts into a single assembly so they move as one unit rather than many.
- Reduce runtime instance creation/destruction churn.


---

### Collect Parts For Min Area

**Collects parts that are within the minimum streaming area (always loaded).**

Identifies parts that must always be present regardless of streaming radius.


---

### Prefetch Gather From Uncollected Regions

**Prefetches data from regions that haven't been collected yet for proactive streaming.**


---

## Garbage Collection (Network)

### Replication Foci GC / GC Loop / Process Deferred GC

**Garbage-collects stale replication data — removes objects that are no longer relevant.**

Cleans up replication state for instances that have been destroyed or streamed out.

**Performance notes:** Normally fast. Can spike after large batch removals.


---

### Part/Model Removals / Foci Removals / Model Removals / Container Removals

**Processes pending removal of parts, models, and containers from the replication system.**

Batch-removes instances that are no longer relevant to any client.

**Performance notes:** Cost scales with how many instances stream out at once — driven by instance density and by players moving far/fast (which streams large regions out).

**What game creators can do:**
- Reduce instance density so fewer instances stream out per move.
- Avoid very fast or very long player movement/teleports, which stream large regions out (and back in) at once.
- Use `Model.ModelStreamingMode` (e.g. Atomic / Persistent) to control how a model streams as a unit.


---

### GC Version Tracking / DeallocateInstanceVersionsAsync / DecayInstanceSyncMap

**Manages version tracking for GC'd instances and cleans up synchronization maps.**


---

## Join Flow

### Join Snapshot / Server Join Snapshot

**Captures the current world state as a snapshot for a joining player.**

Creates the initial state snapshot that will be sent to a newly-joining client. Contains all instances not subject to Streaming system.


---

### GameLoad

**Client-side join-completion handler — fires when the top replication container's 'finished' tag arrives, marking initial game data fully received.**

Records join timing/telemetry, updates join state, signals that the DataModel is loaded, and marks the game as loaded. The initial join snapshot is deserialized and instances are created earlier, not in this scope.

**Performance notes:** Determines load time. Cost = snapshot size.


---

### Container Queue

**Processes the container queue during join — creates instance hierarchies in order.**

Ensures instances are created in the correct parent-child order during the join snapshot application.


---

### Performing player install / Install Remote Player

**Server-side scope for setting up a new player's Player Instance.**


---

### Getting Replication Cache and Queue JDI

**Retrieves cached replication data and queues Join Data Items for the new player.**


---

## Packet Processing

### RakNetUpdate

**Processes incoming and outgoing network packets — the transport layer tick.**

The low-level network update that sends queued outgoing packets and processes received packets. Runs on the network thread.

**Performance notes:** Cost depends on packet volume. High bandwidth usage increases this scope's duration.

**What game creators can do:**
- Reduce network traffic so there are fewer/smaller packets to process — send fewer and smaller replicated updates, lower the rate of frequently-firing `RemoteEvent`s, and reduce the number of network-owned moving parts.


---

### deserializeItem / deserializeClientItem

**Deserializes individual replication items from the network bitstream.**

Decodes received network data back into instance property changes, creations, and deletions. (`decompressBitStream` is a separate step that zstd-decompresses a compressed bitstream before these deserialize it.)

**Performance notes:** CPU-bound deserialization. Cost proportional to received data volume.


---

### Tag deserialized

**Reads a replication flow-control tag item from the stream (e.g. join-milestone markers).**

Decodes an internal replication control tag (not a CollectionService tag); for the join-completion tag, it records join timing and marks that join is complete.


---

### SettingInstanceAlive

**Marks replication-data instances as alive and complete for a replicator during the join flow.**


---

## HTTP & Content

### asyncHttpQueueOnHeartbeat

**Processes the legacy async HTTP request queue.**

Handles a legacy HTTP system; asset requests go through AssetProvider rather than this scope.

**Performance notes:** If many HTTP responses arrive simultaneously, processing them can take time.


---

### contentProviderOnHeartbeat / cacheableContentProviderOnHeartbeat

**Processes content provider asset loading — handles asset request completion.**

Updates the ContentProvider state machine: checks for completed downloads, fires loaded callbacks, and starts new requests. (This describes `contentProviderOnHeartbeat`; `cacheableContentProviderOnHeartbeat` only performs LRU-cache maintenance for cacheable content.)

**Performance notes:** Busy when preloaded content is being processed; preloading more content can increase this heartbeat work while those preloads are active.


---

### HttpRequestAsync / HttpClientReq / HttpClientTC

**Individual HTTP request processing scopes.**


---

### HttpHandleCacheAsyncWriter

**Writes HTTP response data to RbxStorage asynchronously for caching.**


---

### WsClient::workerThreadProcess

**WebSocket client worker thread — processes incoming WebSocket messages.**


---

## Physics & Input Replication

### ParallelPhysicsReceive

**Processes received physics state in parallel — applies server physics corrections.**

Deserializes and applies physics state updates from the server (positions, velocities, CFrames).

**Performance notes:** Cost proportional to the number of physics objects receiving updates.


---

### InputReplicator::sendInputs / InputReplicator::receiveInputs

**Sends local player inputs to the server / receives remote player inputs from the server.**


---

### Sort Priorities / Further Prioritization

**Sorts and refines physics-assembly send priorities for the physics state replication sender.**


---

## Network Quality

### Network Quality Checks / NetworkQualityResponder

**Monitors network connection quality and responds to quality-of-service queries.**

Performs periodic per-connection quality checks and maintenance.


---

## ISR (Instance State Replicator)

### ISRFinalizeStep

**Finalizes the ISR send step — resets the per-frame change tracking after all replicators have sent.**


---

### IsrPartialSortMetrics

**Collects metrics about ISR partial sorting performance.**


---

## Bandwidth & Metadata

### GroupManager / SetManager

**Manages replication groups and sets — organizes instances into replication units.**


---

### Game Services Reporting / GetJobStats

**Reports game service status and collects job statistics for diagnostics.**


---

### Network Quality Checks / Disconnect Cleanup

**Handles disconnection cleanup — removes all replication state for a disconnected client.**


---

### updateMemoryStats

**Updates network memory usage statistics for telemetry.**


---

### GenerateDataBlobsCacheable / GenerateDataBlobsNotCacheable / GenerateDataBlobsCharacterModelRoots / GenerateDataBlobsParts

**Generates serialized data blobs for different categories of replication data.**

Separates replication data into cacheable (static) and non-cacheable (dynamic) categories for efficient serialization.


---

## Data Sender & Packet Details

### Packet Processing

- `processPacket` — processes a single received packet
- `deserializePacket` — deserializes one packet from the bitstream. Low-level packet processing that prepares for **Replicator ProcessPackets**. Advice: send fewer or smaller updates.
- `deserializeBufferedPackets` — deserializes all buffered packets

**What game creators can do:** These scale with how much replicated data the client receives. Reduce it by sending fewer and smaller replicated updates, coalescing frequent property changes, using `RemoteEvent`s sparingly for high-rate data, and using instance streaming to keep fewer instances active.


---

### ISR Details

- `ISR::preProcessFramePropertyChanges` — pre-processes property changes before filtering
- `ISR::stepFilterAndApplyPropertyChanges` — filters and applies property changes for replication
- `Physics Send` / `Physics Send Touches` — sends physics state and touch events


---

### Tracker Data

- `Collect TrackerData` / `Deferred TrackerData update` / `Preserialize TrackerData` — collects, defers, and pre-serializes tracker data for face/body tracking replication. (`Gather Post Join` is a separate instance-streaming step — it collects instance sets that must be sent after a player joins — not tracker data.)


---

### Streaming Details

- `Streaming Invalidated Caches` — invalidates streaming caches after topology changes
- `Resolve Metadata` — resolves deferred replication data and queues newly streamed instances to be added to the replication set
- `UpdateDecayabilityToSyncMap` — updates instance decay eligibility


<br>
<br>

---

<a name="group-ui"></a>
# 📄 `ui.md` — UI Group

<sub>[↑ Contents](#contents)</sub>

The UI group contains scopes for GUI layout, styling, text rendering, tweening, input hit-testing, localization, and UI data structure maintenance. UI work runs primarily on the main thread during the Prepare phase of rendering.

---

## Layout

### Layout

**Runs the UI layout system — computes positions and sizes for all dirty GuiObjects.**

The main layout pass that processes the UI tree. Computes AbsolutePosition and AbsoluteSize for every GuiObject with dirty layout.

**Performance notes:** The most expensive UI scope in complex games. Cost depends on:
- Number of dirty layout nodes (GuiObjects whose properties changed)
- Depth of the UI hierarchy
- Use of UIListLayout, UIGridLayout, UIPageLayout (add layout computation)
- AutomaticSize elements (require multiple layout passes)

**What game creators can do:**
- Minimize property changes that trigger re-layout (Size, Position, Visible)
- Use `LayoutOrder` instead of reparenting to reorder elements
- Avoid deeply nested UI structures
- Use `AutomaticSize = None` when the size is known
- Batch UI updates rather than changing many properties across multiple frames
- Reduce the amount of UI elements being resized or repositioned (via UILayout, TweenService, etc.)
- Consider fixed sizes for BillboardGuis


---

### Post-Layout Event Dispatch

**Dispatches events after layout completes — fires `AbsoluteSize`/`AbsolutePosition` changed signals.**

After layout recomputes positions, this scope fires property-changed signals for any objects whose absolute geometry changed.

**Performance notes:** Cost proportional to the number of elements whose position/size changed. Lua callbacks connected to these signals execute here.


---

### Layout Viewport

**Computes layout for viewport-specific UI elements.**


---

## Tweening

### TweenService

**Steps all active tweens forward by the frame's delta time.**

Advances TweenService:Create() tweens, evaluating their easing functions and firing their completion/cancellation callbacks. Also steps legacy objects tweened via TweenSize/TweenPosition.

**Performance notes:** O(active tweens). Usually fast. Hundreds of simultaneous tweens can add up.

**What game creators can do:**
- Cancel tweens that are no longer visible
- Use fewer simultaneous tweens
- Prefer TweenService over manual lerping in Heartbeat
- Ensure tween completion callbacks do little work


---

### updateGenericTweens

**Updates non-property tweens (NumberSequence animations, etc.).**


---

## Styling (CSS-like system)

### StyleEngine::update / StyleEngine::updateV2 / StyleEngine::updateV3

**Updates the UI style engine — resolves CSS-like style rules for GuiObjects.**

Processes style sheets and resolves property values for elements that match style selectors.

**Performance notes:** Cost depends on the number of styled elements and rule complexity. Games using the style system extensively will see time here.


---

### StyleEngine::addRoot

**Adds a new root element to the style engine's managed tree.**


---

### StyleEngine::update() - sync instances

**Synchronizes styled instance state with the style engine's internal representation.**


---

### StyleEngine::updateSubtrees / StyleEngine::resolveProperties / StyleEngine::applyProperties

**Sub-phases of style resolution: tree traversal, property computation, and property application.**


---

### applying property overrides

**Applies the resolved style property values to each matched instance (the property-application step of the style update).**


---

### StyleAnimator::update / ProcessArchetypeChanges / ComputeAnimatedProperties

**Updates style animations — processes archetype (visual state) transitions and computes interpolated values.**


---

### StyleTree::updateSelectors

**Updates CSS-like selector matching when the instance tree changes.**


---

### StyleCache::update

**Updates the style cache — invalidates and recomputes cached style resolutions.**


---

## Text & Fonts

### load font family metadata

**Loads font family metadata — parses font files to extract available weights, styles, and metrics.**

One-time cost per font family. Cached after first load.


---

### lookup font / request font face load

**Looks up a font face in the font system and initiates loading if not already cached.**


---

### TextRender::render

**Renders text glyphs for a text element — submits glyph geometry to the GPU.**

**Performance notes:** Cost per text element. Many TextLabels with frequently-changing text are expensive.


---

### update GlyphRenderer list

**Updates the list of active glyph renderers for font atlas management.**


---

## Hit Testing & Input

### getBestFitProximityPrompts

**Finds the best proximity prompts to display based on distance and screen position.**

Evaluates all active ProximityPrompts to determine which should be shown on screen.

**Performance notes:** Called from the heartbeat. See Heartbeat group for details.


---

### checkIfLineOfSightClear

**Raycasts to check if a ProximityPrompt has clear line of sight to the player.**

**Performance notes:** One raycast per visible prompt.


---

### doRebuildRenderAndInputLists

**Rebuilds the sorted lists of UI elements for rendering and input hit-testing.**

Called when the UI hierarchy changes (elements added, removed, or re-parented).

**Performance notes:** Sort of visible elements. Expensive with thousands of UI elements.

**What game creators can do:**
- Reduce the total number of on-screen GUI objects.
- Avoid frequently changing `ZIndex` or re-parenting GUI objects — each change rebuilds these lists.
- Prefer `ScreenGui` over many `BillboardGui`s where possible (billboard sorting is camera-dependent and costs more).


---

### reSortInputList

**Re-sorts the UI input list after Z-order changes.**

**Performance notes:** Also re-runs as the camera moves when camera-dependent GUIs (BillboardGuis) are present; cost scales with the number of on-screen GUI objects.

**What game creators can do:**
- Reduce `ZIndex` churn and the number of on-screen GUI objects.
- Reduce `BillboardGui` count — these force a re-sort whenever the camera moves.


---

### GuiService::getSelectionGroupIntersection

**Computes the intersection of gamepad selection groups for navigation.**


---

### Move Gamepad Selection

**Moves the gamepad UI selection cursor in a direction (up/down/left/right).**

Uses spatial queries to find the next selectable element.


---

### Rebuild Quadtree / Update Quadtree / Rebuild Z-order list

**Maintains the spatial quadtree used for fast UI hit-testing and the Z-order rendering list.**

**Performance notes:** Fires when UI elements move or are added/removed. Frequent changes trigger frequent rebuilds.

**Rebuild Z-order list** sorts the Z-order of UI elements (an internal term, distinct from `GuiObject.ZIndex`) to prevent tearing.

**What game creators can do:**
- Reduce the amount of UI elements with the same ZIndex


---

### UIQuadTree::processUpdateList

**Processes pending spatial updates in the UI quadtree.**


---

## RelativeGui (3D-positioned UI)

### RelativeGui input / RelativeGui gesture / RelativeGui build vector

**Processes input, gestures, and child-list rebuilds for a RelativeGui — a GuiObject that positions its children in relative 2D screen space.**

**Performance notes:** Cost per visible 3D-positioned GUI element.


---

## Localization

### LocalizationService::attemptTranslation / attemptLocalization / attemptDynamicTranslation

**Attempts to translate UI text using localization tables.**

Looks up text strings in the localization system and replaces them with translated versions.

**Performance notes:** Usually fast (hash table lookup). Can be expensive if triggered frequently on many elements.


---

### LocalizationService::invalidateAutoTranslations

**Invalidates cached auto-translations when locale changes or translation tables update.**


---

## Text Scraper

### TextScraper::ScraperJob::step / TextScraper::ClientScraperJob::stepDataModelJob / TextScraper::ServerScraperJob::stepDataModelJob

**Scrapes text from the game for automatic localization table generation.**

Background job that drains the queues of already-collected untranslated strings and parameter text for localization. The UI hierarchy is walked upstream when strings are enqueued, not in this step.

**Performance notes:** Background job, doesn't block rendering. But can compete for CPU time.


---

## Path2D

### Path2D_Rebuild

**Rebuilds a Path2D's spline and mesh geometry when it is marked dirty, during the UI render pass.**

Recomputes the path tessellation for rendering.

**Performance notes:** Cost depends on path complexity (number of control points, tessellation quality).


---

## Rendering (within UI group)

### fillGuiVertices

**Fills GPU vertex buffers with UI element geometry (quads, borders, gradients).**

Generates the vertex data that the GPU renders for all visible UI elements.

**Performance notes:** O(visible UI elements). Complex elements (rounded corners, gradients) generate more vertices.

**Labels:** `gui count` indicates the number of LayerCollectors visible.

**What game creators can do:**
- If cost is high, reduce the amount, density, or screen space of UI elements
- If there are too many `Process GuiEffect` labels, reduce use of `UIGradient` and `UICorner` on text labels


---

### Process GuiEffect

**Processes GuiEffect instances (UIBlur, etc.) during UI rendering.**


---

## UIDragDetector

### UIDragDetector::dragStart / UIDragDetector::dragContinue / UIDragDetector::dragEnd

**UIDragDetector lifecycle — handles the start, continuation, and end of a UI drag interaction.**


---

### UIDragDetector::restartDrag

**Restarts a drag operation (e.g., after a constraint change during drag).**


---

### UIDragDetector::applyCallbacksInDragContinue

**Invokes Lua callbacks during drag continuation.**


---

### UIDragDetector::applyDragBounds / boundDragByBoundingRect / boundRotate / boundTranslateLine / boundTranslatePlane

**Applies drag bounds constraints — restricts drag movement to defined regions, lines, planes, or rotations.**


---

### GuiObject::processWithDragDetector

**Processes input on a GuiObject that has a UIDragDetector attached.**


---

## Dynamic Localization

### LocalizationService::attemptDynamicTranslation

**Attempts dynamic runtime translation of text content.**


---

### LocalizationService::attemptLocalization

**Attempts to localize a text string using the active localization table.**


---

## Text Scraping

### TextScraper::ClientScraperJob::stepDataModelJob / TextScraper::ServerScraperJob::stepDataModelJob

**Background text scraping jobs — drain the queues of already-collected untranslated strings and upload them to the auto-localization service. The UI hierarchy is walked upstream when strings are enqueued.**


<br>
<br>

---

<a name="group-animation"></a>
# 📄 `animation.md` — Animation Group

<sub>[↑ Contents](#contents)</sub>

The Animation group contains scopes related to skeletal animation playback, retargeting, inverse kinematics, and the animation graph system. These scopes fire during the physics/simulation step at the fixed rate (typically 60 Hz).

---

## Related Frame-Level Scopes (in Physics group)

The following scopes in the Physics group orchestrate animation at the frame level:
- `stepAnimationPrepare` — prepares animation state for the current step
- `stepAnimation` — runs all Animator instances (parallel or serial)
- `stepIK` — runs IK solvers after animation

---

## AnimationTrack::stepRetargeted

**Evaluates one animation track with retargeting applied — the main per-track step function.**

Advances an AnimationTrack by one time step, evaluates keyframes/curves at the current time, applies retargeting to map the source animation skeleton to the target rig, and accumulates the result into the pose buffer.

**Nesting:** Runs under `AnimatorParallelManager::stepAll` (parallel dispatch is per-Animator; each Animator steps all of its own tracks), which runs within the frame-level `stepAnimation` scope.

**Performance notes:** Called once per playing AnimationTrack per frame. Cost depends on:
- Number of keyframes/bones in the track
- Whether retargeting is active (adds IK solve overhead)
- Blending complexity (multiple overlapping tracks)

**What game creators can do:**
- Reduce the number of simultaneously playing animation tracks per character
- Use animation priorities to avoid unnecessary blending
- Stop animations that aren't visible on screen


---

## AnimationTrack::clipApply

**Applies the raw clip data (keyframes/curves) to produce joint transforms for one time sample.**

Evaluates the animation clip at the current playback time to produce local joint transforms. This is the raw keyframe interpolation step before retargeting.

**Performance notes:** Fast for simple animations. Can be expensive for dense keyframe sequences or many bones.


---

## stepRetargeted::accumulators

**Accumulates retargeted joint transforms into the final pose blend buffer.**

After retargeting computes the joint transforms for this track, this phase blends them into the accumulated pose using the track's weight and blend mode.

**Performance notes:** Fast — simple weighted accumulation.


---

## AnimationTrack::refreshTargetRig

**Rebuilds the retargeting mapping when the target rig changes (e.g., avatar changes body shape).**

This scope wraps the whole function, so it fires on every retargeted track step. Most frames it early-outs cheaply; the expensive work — scanning the rig hierarchy and rebuilding the retarget map — only runs when the target rig actually changes (avatar swap, body-shape change).

**Performance notes:** Expensive but rare — only triggers when the rig actually changes (avatar swap, body shape change). Should not fire every frame.

**What game creators can do:**
- Avoid frequently swapping character rigs mid-game
- If rig changes are expected, batch them rather than changing bones incrementally


---

## refreshTargetRig::checkCache

**Checks if the cached retargeting data is still valid for the current rig.**

Fast validation step that checks whether the existing retarget map can be reused or needs rebuilding.

**Performance notes:** Negligible. Early-exit path.


---

## refreshTargetRig::newRetargeter

**Builds a new retargeter for the current source-to-target rig pair — set up when an animation authored for one rig is played on a differently-proportioned rig.**

Constructs the retargeting object that maps one skeleton's proportions to another. This is the expensive path when the cache is invalid.

**Performance notes:** Moderate cost, but should be rare.


---

## AnimationTrack::createSrcRigForTrack

**Constructs the source rig representation from the animation clip's skeleton data.**

Builds a source skeleton model from the animation asset's bone structure. Required for retargeting to know what skeleton the animation was authored for.

**Performance notes:** One-time cost per unique animation-rig pairing. Cached after first creation.


---

## KeyframeSequence::apply

**Applies a KeyframeSequence (legacy animation format) to produce joint transforms.**

Evaluates the legacy KeyframeSequence format at the current time. This is the older animation format (pre-CurveAnimation).

**Performance notes:** Per-track cost. Legacy format is slightly less efficient than CurveAnimation.


---

## CurveAnimation::apply

**Applies a CurveAnimation (modern animation format) to produce joint transforms.**

Evaluates the modern curve-based animation format. More efficient and higher quality than KeyframeSequence.

**Performance notes:** Per-track cost. Generally fast with binary search on sorted keyframe arrays.


---

## ClipEvaluator::evaluate

**Evaluates a generic animation clip — dispatches to the appropriate format handler.**

Entry point for clip evaluation that delegates to either KeyframeSequence::apply or CurveAnimation::apply based on the clip type.

**Performance notes:** Adds minimal overhead on top of the actual clip evaluation.


---

## IkControlManager::update

**Updates all IkControl instances — runs inverse kinematics for each active IK chain.**

Iterates over all active IkControl objects and solves their IK chains. IK is solved after forward kinematics (animation) to apply procedural constraints.

**Nesting:** Called inside `stepIK` at the frame level.

**Performance notes:** Cost = O(IK controls × chain length × iterations). Expensive with:
- Many active IkControl instances
- Long IK chains (many bones)
- High iteration counts

**What game creators can do:**
- Limit the number of active IkControl instances
- Disable IK on characters that are far from the camera
- Use shorter IK chains when possible
- Reduce `IkControl.SmoothTime` to converge faster with fewer iterations


---

## AnimationRig::bindRigToR15Joints

**Binds the animation rig to the R15 joint hierarchy of a character model.**

Scans the character model's Motor6D joints and maps them to the animation rig's bone names. This establishes the connection between animation data and the actual rig.

**Performance notes:** One-time cost per character. Not a per-frame scope.


---

## R15ToR15Retargeter::initialize / R15ToR15Retargeter::newInitialize

**Initializes the R15-to-R15 retargeting system for a source/target rig pair.**

Computes the skeletal proportions ratio between source and target rigs, and builds the joint-level retarget transforms.

**Performance notes:** One-time cost per unique rig pair. Cached.


---

## retarget

**Performs the actual retargeting computation — maps source joint transforms to target proportions.**

The core retargeting algorithm that takes source-skeleton local transforms and produces target-skeleton local transforms accounting for different limb lengths and proportions.

**Performance notes:** Per-track, per-frame cost when retargeting is active. Moderate — involves per-bone matrix math.


---

## RetargetFK

**Forward kinematics phase of retargeting — transforms joints from source to target space.**

Applies the FK (forward kinematics) portion of the retarget: straightforward joint-to-joint mapping with proportion scaling.

**Performance notes:** Fast linear pass over the skeleton.


---

## RetargetIK

**Inverse kinematics phase of retargeting — corrects end-effector positions after FK mapping.**

After FK retargeting, IK corrections are applied to maintain foot/hand contact positions. This ensures that retargeted animations still have feet touching the ground.

**Performance notes:** More expensive than FK phase due to IK correction passes. Only runs when the retargeter detects that IK correction is needed.


---

## AnimationGraph::update

**Updates the animation graph state machine — evaluates transitions and advances node states.**

Processes the animation state machine: checks transition conditions, advances active states, and determines which animation nodes should be evaluated this frame.

**Performance notes:** Cost depends on graph complexity (number of states, transitions, blend trees). Typically fast for simple graphs.


---

## AnimationGraph::evaluate

**Evaluates the animation graph output — produces the final blended pose from all active nodes.**

Walks the active animation graph nodes and blends their outputs together to produce the final skeletal pose. This is where multi-layer blending and additive animations combine.

**Performance notes:** Cost proportional to the number of active nodes in the graph. Deep blend trees with many simultaneous animations are more expensive.


---

## ModelSkinningTransformsView::reset

**Resets the skinning transforms view, clearing cached bone transforms.**

Invalidates the cached skinning matrix palette, forcing a recomputation on the next frame. Called when the model's skeleton changes.

**Performance notes:** Negligible one-time cost.


---

## ModelSkinningTransformsView::rebuildView

**Rebuilds the complete skinning transforms view for a model — maps bones to GPU-ready transform matrices.**

Reconstructs the full bone-to-transform mapping for skinned mesh rendering. This produces the matrix palette that the GPU uses for vertex skinning.

**Performance notes:** Cost proportional to bone count. Triggers when rig hierarchy changes.


<br>
<br>

---

<a name="group-sound"></a>
# 📄 `sound.md` — Sound Group

<sub>[↑ Contents](#contents)</sub>

The Sound group contains scopes for audio processing: audio engine updates, spatial audio panning and occlusion, instance stepping, reverb, and speech-to-text encoding. While some per-frame tasks run on the main thread - such as passing spatial position info from the datamodel to the audio engine - core processing is handled by a few dedicated threads, including the mixer and asset loading threads.

---

## Audio Engine Core

### updateFmod

**Ticks the audio engine — passes pending datamodel changes into FMOD.**

The main audio engine update that passes information from the datamodel into FMOD, such as 3D positions and play/stop commands. Audio mixing and output submission happen outside this scope.

**Performance notes:** Cost primarily depends on the number of active `Sound` instances or `AudioPlayer`s. Can spike when many sounds start simultaneously.

**What game creators can do:**
- Reduce the number of simultaneously playing sounds
- Use SoundGroups with volume limits
- Set `RollOffMaxDistance` to cull distant sounds
- Avoid starting many sounds on the same frame


---

### updatePanning

**Updates 3D spatial panning for all active sound emitters based on listener position.**

Computes the spatial positioning (left/right panning, distance attenuation, Doppler) for each active 3D sound relative to the listener (camera or AudioListener).

**Performance notes:** O(active 3D sounds). Usually fast. Can be noticeable with hundreds of simultaneous spatial sounds.


---

### stepInstances / stepInstances_Parallel

**Steps all active sound instances — advances their state machines and checks for completion.**

Updates each playing sound's state: checks if it finished, handles looping, and updates properties that changed from Lua.

**Performance notes:** O(active Sound instances).

**What game creators can do:**
- Reduce the amount of sounds in active playback


---

### updateReverb

**Updates reverb parameters based on the listener's environment.**

Computes environmental reverb settings by sampling the acoustic environment around the listener. May involve raycasts for room-size estimation.

**Performance notes:** Usually fast. Can be expensive if acoustic simulation is enabled with high-quality settings.


---

### calculateTrace

**Computes an audio occlusion trace between a sound source and the listener — traces the path through the scene to work out how much the sound is blocked or muffled by geometry.**

Runs for spatially-simulated sounds that use audio occlusion; on the synchronized path it reads the current occlusion state of the world. It fires per source–listener pair, so cost scales with the number of occlusion-simulated sounds.

**Performance notes:** Can widen when many spatial sounds use occlusion simulation at once.

**What game creators can do:**
- Limit how many sounds use audio occlusion / spatial simulation simultaneously.
- Lower occlusion/acoustic simulation quality where full fidelity isn't needed.


---

### FMOD::Output::mix

**Evaluates the audio DSP graph — mixes all audio producers and effects into a final buffer for the soundcard.**

The workhorse of the audio engine. Running on a dedicated mixer thread rather than per-frame, it topologically sorts all DSP nodes in the audio graph, applies pending parameter changes, and walks the graph front-to-back to sum the results into an output buffer delivered to the hardware.

**Performance notes:** Scales with the number of active audio producers and DSP effects, as well as the volume of property changes since the last mix. Because the soundcard reads from the output buffer on a strict hardware timer regardless of whether it is full, taking too long in this scope causes buffer underruns — heard in-game as audio crackling or sputtering.

**What game creators can do:**
- Limit the number of simultaneously active audio producers and DSP effects


---

## Audio Channel Operations

### release

**Releases a sound resource — frees the underlying FMOD sound object.**

Called when a Sound instance is destroyed or its asset changes. One-time cleanup cost.

**Performance notes:** Negligible per-call. Batch releases (destroying many sounds at once) can spike.


---

### FMODPlaybackChannel::createFMODChannel_

**Creates a playback channel — initializes a hardware channel for a sound to play on.**

Called when a Sound starts playing. Allocates an audio channel and configures it with the sound's properties.

**Performance notes:** Moderate per-call (involves audio channel allocation). Avoid starting hundreds of sounds simultaneously.


---

### FMODPlaybackChannel::setPlaying

**Starts/stops playback on an audio channel.**

Transitions a sound to the playing or stopped state.


---

### FMODPlaybackChannel::setTimePosition

**Seeks an audio channel to a specific time position.**

Sets the playback cursor. Can involve decoder seek operations for compressed audio.


---

## Speech-to-Text (SpeechEncoder)

These scopes fire when using voice input or speech-to-text features. They encode captured audio and send it to the STT service.

### SpeechEncoder::beginEncode

**Begins a speech encoding session — initializes the encoder for capturing speech.**


---

### SpeechEncoder::encode

**Encodes one chunk of audio data for speech-to-text processing.**

**Performance notes:** Runs only while voice input / speech-to-text is active. It executes on the **Sound** job — it drains microphone audio that was captured for speech-to-text and encodes it — so it appears on the Sound job's timeline, not on the real-time audio thread. Cost is CPU work proportional to the amount of speech captured.


---

### SpeechEncoder::endEncode

**Ends the speech encoding session — finalizes the encoded audio stream.**


---

### SpeechEncoder::encodeSpeech_DEPRECATED

**Alternate speech encoding path (retained for compatibility).**


---

### SpeechEncoder::makeRequestBody

**Constructs the HTTP request body for the STT API call.**


---

### SpeechEncoder::makeCallback

**Creates the response callback for the STT API request.**


---

### SpeechEncoder::makeAndSendQuery

**Sends the encoded speech to the speech-to-text server.**


<br>
<br>

---

<a name="group-input"></a>
# 📄 `input.md` — Input Group

<sub>[↑ Contents](#contents)</sub>

The Input group contains scopes related to processing player input — keyboard, mouse, touch, and gamepad events. These scopes fire during the input processing phase of each frame, before rendering.

---

## ProcessInput

**Top-level scope for processing all pending input events from the UserInputService on the render step.**

This is the main entry point for input processing each frame. It dequeues all pending input events (from the platform input queue) and routes them through the input pipeline: UserInputService → ContextActionService → GUI hit testing.

**Nesting:** Runs during the render preparation phase. Contains ProcessUIActions and ProcessActions.

**Performance notes:** Usually fast. Can be slow if:
- Many input events are queued (e.g., rapid mouse movement with high polling rate)
- Lua callbacks connected to InputBegan/InputChanged/InputEnded are expensive
- GUI hit testing is expensive due to deeply nested UI hierarchies

**What game creators can do:**
- Keep InputBegan/InputChanged/InputEnded callbacks lightweight
- Do minimal work in the callback itself — defer heavier computations off the input event (e.g. with `task.defer`/`task.spawn`, or spread the work across later frames) rather than running them inline
- Avoid deep UI hierarchies that make hit testing expensive
- Use ContextActionService bindings instead of raw input events when possible


---

## ProcessUIActions

**Routes input events through ContextActionService UI-priority bindings.**

Processes state changes for on-screen touch/GUI buttons created by ContextActionService (via `SetTitle`/`SetImage`/`GetButton`-style CAS action buttons), updating each button's action state and firing its connected bindings. This is CAS button input, distinct from keyboard/gamepad action routing.

**Nesting:** Inside ProcessInput.

**Performance notes:** Cost proportional to the number of bound UI actions. Fast in typical games.

**What game creators can do:**
- Unbind actions when they're not needed (e.g., when a menu is closed)
- Avoid binding many actions at the same priority level


---

## ProcessActions

**Routes input events through ContextActionService game-world bindings.**

Processes input events against non-UI actions. This is where movement, combat, and other gameplay bindings fire.

**Nesting:** Inside ProcessInput, after ProcessUIActions.

**Performance notes:** Similar to ProcessUIActions. Expensive if many actions are bound or if their Lua callbacks do heavy work.


---

## ProcessActionsInternal

**Internal dispatch of action processing — iterates through bound actions and invokes matching handlers.**

The inner loop of action processing. Matches input events against bound action keycodes and invokes the corresponding Lua callback functions.

**Nesting:** Inside ProcessActions.

**Performance notes:** Per-event cost. Multiple bound actions with overlapping keys increase processing time.


---

## UIS:InputBeganChangedEnd

**Fires UserInputService.InputBegan, InputChanged, or InputEnded signals to Lua.**

This scope wraps the Lua signal dispatch for raw input events on UserInputService.

**Performance notes:** Cost depends on the number of connections and their callback complexity.


---

## UserInputService::processGestures

**Processes pending touch gesture events (pinch, pan, rotate, tap, etc.).**

Dequeues gesture events from the platform gesture recognizer and fires corresponding Lua signals (TouchPinch, TouchPan, etc.).

**Performance notes:** Only relevant on touch devices. Fast unless many gesture connections exist with expensive callbacks.


---

## NullTool::onMouseHover

**Performs hover detection — raycasts into the 3D scene under the mouse cursor to detect ClickDetectors and hoverable parts.**

Runs each frame while no tool is equipped and the mouse moves. Performs a raycast from the camera through the mouse position to find parts with ClickDetectors or hover state.

**Performance notes:** One raycast per frame when hovering. Normally fast, but can be expensive in dense scenes with many parts.

**What game creators can do:**
- Parts with ClickDetectors add to raycast cost; remove unused ClickDetectors
- Complex collision geometry (MeshParts with high-fidelity collision) increases raycast cost


---

## NullTool::onMouseMove

**Handles mouse-move while a ClickDetector already has input capture — updates the activated cursor and forwards the moved ray to that detector.**

Runs only when a ClickDetector currently holds capture (mouse-down drag). It refreshes the activated cursor icon and calls the detector's moved-ray update, releasing capture if the ray no longer targets it. This is drag tracking on a captured detector, not passive hover/highlight during free mouse movement.

**Performance notes:** Minimal cost per frame.


---

## DEPRECATED_ContextActionService::tryProcess

**Alternate ContextActionService input processing path.**

Still fires in some configurations for backward compatibility.

**Performance notes:** Typically negligible.


---

## GuiLayerCollector::processGesture

**Routes a gesture event through the GUI input system to find which UI element should handle it.**

Hit-tests gesture events against the GUI hierarchy to determine which GuiObject should receive the gesture.

**Performance notes:** Cost depends on the number and depth of active ScreenGuis and their elements.


---

## GuiLayerCollector::processDescendants

**Walks the GUI hierarchy to find input targets for mouse/touch events.**

Traverses the Z-ordered list of GUI elements to determine which element the input event should target. Uses the UI quad tree for spatial acceleration.

**Performance notes:** Expensive with very deep or numerous GUI hierarchies. The quad tree helps, but thousands of overlapping elements still cost.

**What game creators can do:**
- Reduce the total number of visible GuiObjects
- Set `Active = false` on elements that don't need input
- Avoid deeply nested GUI structures


---

## HapticMixer.run

**Mixes haptic motor intensities from multiple sources and applies them to the gamepad.**

Runs per-frame to combine haptic feedback requests and output the final motor intensities to connected haptic devices (gamepad rumble).

**Performance notes:** Negligible. Only runs when haptic devices are present.


---

## HapticService.getNextHapticIntensities

**Collects pending haptic intensity requests from game scripts.**

Gathers all haptic motor intensity values that scripts have set since the last frame.

**Performance notes:** Negligible.


---

## updateControllers

**Updates SDL gamepad controller state — polls connected controllers for button/axis changes.**

Queries the platform input layer for gamepad state changes and generates input events for any buttons pressed/released or axes moved.

**Performance notes:** Negligible. Cost is O(connected controllers).


---

## PollAllInput / PollRawInput / PollLegacyInput

**Polls the platform input system for new events (Windows client) — raw input, legacy input, or all input sources at once.**

**Performance notes:** Negligible in normal use. Fires during the input polling step on the Windows desktop client.


<br>
<br>

---

<a name="group-navigation"></a>
# 📄 `navigation.md` — Navigation Group

<sub>[↑ Contents](#contents)</sub>

The Navigation group contains scopes for the PathfindingService navmesh system: navmesh generation (heightfield generation, region building, and mesh construction), tile management, and path computation. Navmesh work runs on dedicated worker threads; pathfinding queries may run on the main thread or parallel threads.

---

## Path Computation

### computePath

**Computes a navigation path from start to goal on the navmesh.**

The main pathfinding query. Searches the navigation mesh to find the shortest walkable path between two points. Returns a series of waypoints.

**Performance notes:** Cost depends on path length and navmesh complexity. Long paths across large navmeshes are expensive. Can spike when many scripts call `PathfindingService:CreatePath()` simultaneously.

**What game creators can do:**
- Cache paths and reuse them rather than recomputing every frame
- Limit pathfinding frequency (compute every 0.5-1 second, not every frame)
- Keep path distances reasonable (don't pathfind across the entire map)
- Reduce PathfindingModifier usage on large areas
- Reduce the number and world extents of `Path:ComputeAsync()` calls; reuse paths for multiple agents that start/end at approximately similar locations


---

## Navmesh Generation

Navmesh generation runs in the background when the world changes. These scopes fire on the navigation worker thread.

### navmeshWorkerFunction

**Main navmesh worker loop — processes dirty tiles and regenerates their navigation meshes.**

The background thread that processes the navmesh generation queue. Picks up dirty tiles (areas where geometry changed) and regenerates their navigation data.

**Performance notes:** Runs in the background and shouldn't block the main thread. However, heavy generation work can compete for CPU time with other threads.


---

### rasterizeTile

**Updates navigation tiles needed for a pathfinding request, usually followed by computePath which requires those tiles be up-to-date.**

Updates the navigation tiles required for a pathfinding request, converting the parts and terrain within a tile into a heightfield representation. Usually followed by `computePath`, which requires those tiles to be up-to-date.

**Performance notes:** Cost depends on the number and complexity of parts in the tile. Dense areas with many parts take longer.

**What game creators can do:**
- Reduce the number of pathfinding tile invalidations, as this causes those paths to need recomputing. This is caused by non-navigable parts moving.


---

### getDirtyTilesForRegion

**Identifies which navigation tiles overlap a changed region and need regeneration.**

When parts are added/removed/moved, this scope determines which tiles are affected.


---

## Rasterization Sub-Phases

### preprocessParts

**Preprocesses part geometry for rasterization — collects and transforms part shapes.**

Gathers all parts in the tile's bounds and prepares their collision geometry for the rasterizer.


---

### preprocessTerrain

**Preprocesses terrain for rasterization — extracts terrain geometry in the tile's bounds.**


---

### ReadingTerrainBox

**Reads terrain voxel data for a region — queries the terrain spatial structure.**


---

### TerrainMeshWork

**Converts terrain voxels to triangle mesh for rasterization.**


---

### getPrimitivesOverlapping

**Broadphase query — finds all physics primitives overlapping a pathfinding tile's bounds.**

Finds all physics primitives overlapping the tile bounds using broadphase queries.

**What game creators can do:**
- Reduce the part count


---

### rasterize / rasterize/triangleMesh / rasterize/Terrain

**Core rasterization — converts triangles and terrain into heightfield voxels.**

The core rasterization step that processes triangles and terrain cells. The most expensive per-tile step.

**Performance notes:** Cost proportional to triangle count × tile resolution.


---

## Navmesh Construction

### buildCompactHeightfield

**Builds a compact heightfield from the rasterized data — compresses the voxel grid.**

Converts the raw heightfield into a compressed representation that the region builder can work with.


---

### buildOffMeshConnections

**Builds automatic off-mesh connections — jump, drop, and climb links derived from the heightfield geometry.**

Generates the engine's automatic jump/drop/climb connections across gaps and ledges. Creator-defined `PathfindingLink` instances are added in a separate step outside this scope.


---

### buildRegions

**Builds navigable regions by flood-filling connected walkable areas.**

Groups connected walkable voxels into contiguous regions that will become navmesh polygons.


---

### buildContours

**Builds contour outlines of navigable regions — extracts region boundaries.**

Traces the edges of each region to create polygon outlines for the navmesh.


---

### buildPolyMesh

**Builds the polygon mesh from contours — creates the final navmesh triangulation.**

Triangulates the region contours into a polygon mesh suitable for pathfinding queries.


---

### buildNavMeshTiles

**Assembles the final navmesh tile from the polygon mesh — creates the runtime query tile data.**

Packages the generated navmesh into the runtime format for pathfinding queries.


<br>
<br>

---

<a name="group-voxel"></a>
# 📄 `voxel.md` — Voxel Group

<sub>[↑ Contents](#contents)</sub>

The Voxel group contains scopes for the Smooth Terrain voxel system: reading/writing terrain data, LOD management (downsampling/upsampling), serialization, geometry generation for rendering, and terrain editing operations. Terrain operations run on both the main thread and worker threads.

---

## Reading & Writing

### read / read_vjm

**Reads voxel data from the terrain spatial structure for a given region.**

Fetches terrain cell data (material, occupancy) from the internal storage. The `vjm` variant uses the Voxel Job Manager path.

**Performance notes:** Fast for small regions. Cost proportional to the number of cells read.


---

### write

**Writes voxel data into terrain storage — the low-level write used both for terrain edits and for the writes that occur when terrain is loaded or streamed in.**

Applies terrain writes to the internal storage: player/script edits (fill, dig, paint) as well as writes produced when terrain data is loaded from a place or streamed in. Triggers invalidation of affected LOD levels and rendering geometry.

**Performance notes:** Fast per-cell. Batch writes are efficient. Triggers downstream work (LOD rebuilds, mesh regeneration).

**What game creators can do:**
- Batch terrain edits into fewer `WriteVoxels` calls
- Avoid modifying terrain every frame
- Use larger brush sizes instead of many small edits


---

### readRegionEmptyLod0

**Reads a terrain region, returning quickly if LOD0 shows the region is empty.**

Optimized read path that can early-exit when the finest LOD level indicates no terrain exists in the region.


---

## LOD Management (Downsampling/Upsampling)

### downsample / downsampleAll / downsampleAllVJM

**Downsamples terrain data to coarser LOD levels.**

Coarser LODs are produced in two situations: when terrain is modified at the finest level (so the coarser LODs reflect the change), and on demand when a region is read at a coarser LOD than is currently stored. This scope generates the lower-resolution representations.

**Performance notes:** Cost proportional to the affected region size. Large terrain edits trigger more downsampling.


---

### downsampleSdf

**Downsamples the SDF (Signed Distance Field) representation of terrain.**


---

### asyncDownsample / asyncUpsample

**Deferred, time-budgeted terrain LOD operations (downsampling / upsampling).**

Run from the terrain DataModel job with a per-frame time budget, so a large LOD backlog is spread across frames rather than done all at once. "async" here means budgeted/deferred, not a separate background thread.


---

### asyncDestroy

**Asynchronously destroys terrain data for regions that have been cleared.**


---

### rebuildAllLods

**Rebuilds all LOD levels from scratch — expensive full-terrain LOD regeneration.**

Called after major terrain topology changes or when loading a place.

**Performance notes:** Very expensive. Should only happen on place load or major terrain operations.


---

## SDF Operations

### compressedToSdf / sdfToCompressed / packSdf / unpackSdf

**Converts terrain between compressed storage format and SDF (Signed Distance Field) representation.**

The terrain system stores data in a compressed format internally but operates on SDF for smooth surface computation.


---

### unclampSdfBox / unclampSolid / unclampWater

**Unclamps SDF values for specific material types — restores full-range values from clamped storage.**


---

### upsampleCompressed / upsampleSolid

**Upsamples compressed terrain data to finer resolution levels.**


---

## Geometry Generation

### generateGeometry / generateGraphicsGeometry

**Generates triangle meshes from terrain voxel data for rendering.**

Generates renderable triangle meshes from terrain data.

**Performance notes:** One of the more expensive terrain operations. Cost proportional to the number of terrain cells that need mesh regeneration.


---

### generateGraphicsGeometryPacked / generateShadowGeometryPacked / generatefeedbackGeometryPacked

**Generates packed geometry for different render passes: main rendering, shadow maps, and virtual texture feedback.**

Optimized geometry generation for specific purposes (shadow geometry is simpler, feedback geometry is lower-resolution).


---

### compactMeshVertices

**Compacts mesh vertex arrays — removes unused vertices after geometry generation.**


---

### addEdges

**Builds the per-face edge-to-triangle adjacency map used to group terrain surface triangles into UV islands during texture unwrapping.**


---

## Spatial Structure Operations

### growOctree

**Adds another level to the terrain octree (increasing its depth) so it can hold coarser LOD levels.**

Expands the spatial data structure to accommodate terrain in a new area.

**Performance notes:** Rare but expensive when it happens. Triggers memory reallocation.


---

### isEmpty

**Checks if a terrain region is empty (no non-air cells).**

Fast early-exit check used by many operations.


---

### isHighLod / getAvailableNonEmptyLod

**Queries the LOD state of terrain regions.**


---

### getCellLevel / getNonEmptyIds / getNonEmptyRegions / getNonEmptyRegionsInside

**Spatial queries on the terrain structure — finds non-empty cells or regions.**


---

## Serialization

### serialize / serializeTerrainRegion / deserialize

**Serializes/deserializes terrain data for saving, networking, or streaming.**

Converts terrain data to/from wire format for replication or file storage.

**Performance notes:** Cost proportional to terrain volume being serialized.


---

### unzipCells

**Decompresses terrain cell data from compressed storage.**


---

## Terrain Editing Operations

### erase / eraseHigherResolution

**Erases terrain — sets cells to air material.**

`eraseHigherResolution` additionally clears finer LOD data that may exist above the erased level.


---

### sweep

**Runs the fast-sweep pass that propagates signed-distance-field values across the terrain voxel grid (part of unclamping SDF data for surface meshing/LOD).**


---

### processModifyOperations / processModifyWrite / processReadOperations

**Processes batched terrain modification and read operations from the job queue.**

The terrain system queues operations and processes them in batches for consistency.


---

## Texture Unwrapping

### getUnwrapData / getUnwrapDataHeightmap / getUnwrapDataOriented

**Generates texture unwrapping data for terrain — computes UV coordinates for virtual texturing.**

Calculates how terrain surface textures should be mapped. Used by the virtual texture system.


---

### placeUnwrappedCharts / buildAtlasTris

**Packs unwrapped texture charts into the atlas and builds atlas triangle data.**


---

## Miscellaneous

### GridJob

**The main terrain grid processing job — coordinates all per-frame terrain work.**

Top-level scope for the terrain worker thread's frame processing.


---

### On Fragment Loaded

**Processes a terrain fragment that has finished loading from disk or network.**


---

### Decode PBC Chunk

**Decodes a PBC (Packed Binary Chunk) of terrain data received from the network.**


---

### UpdateRegionChangeListeners

**Notifies listeners about terrain region changes (rendering, physics, streaming).**


---

### approximateRegionAtLevel

**Computes the bounding region (AABB) enclosing all terrain chunks present at a given LOD level.**


---

### fixupLimits

**Corrects sharp SDF transitions during unclamping — converts adjacent full/empty (±max) voxel pairs into ±0.5 boundary values.**


<br>
<br>

---

<a name="group-voice"></a>
# 📄 `voice.md` — Voice Group

<sub>[↑ Contents](#contents)</sub>

The Voice group contains scopes for the voice chat system: audio capture, processing, mixing, voice transport, device management, stats monitoring, and VoiceChat lifecycle management. Voice work runs on dedicated audio threads and the main thread.

---

## Core Operations

### GenerateAndRunOperations

**Top-level voice operation loop — generates and executes pending voice operations (join, leave, mute, subscribe).**

The main voice chat state machine step. Processes queued operations that transition the voice system between states.

**Performance notes:** Usually fast. Can spike when many players join/leave voice simultaneously.


---

### UpdateRecordedBuffer / RobloxAudioDevice::UpdateRecordedBuffer / RecordingDeviceManager::UpdateRecordedBuffer

**Captures audio from the microphone and buffers it for processing.**

Reads raw audio samples from the recording device into the voice processing pipeline.

**Performance notes:** Runs at the audio callback rate (~10 ms frames). Must complete within the callback period to avoid dropouts.


---

### ClientOperation::run

**Executes one voice client operation (join room, publish stream, subscribe to peer, etc.).**


---

## Audio Processing

### CustomAudioProcessing::ProcessStream

**Processes a voice audio stream — applies noise suppression, echo cancellation, and gain control.**

The audio DSP pipeline for voice: runs voice audio processing modules (AEC, NS, AGC) on captured or received audio.

**Performance notes:** CPU-intensive signal processing. Runs in real-time on audio threads. Should stay under a few milliseconds.


---

### CustomAudioProcessing::writeToAudioSink

**Writes processed captured microphone audio into the capture sink that feeds the voice send/encode pipeline.**


---

### CustomAudioProcessing::CustomAudioProcessing

**Initializes the custom audio processing pipeline.**

One-time setup cost.


---

## Audio Mixing

### CustomAudioMixer::Mix

**Mixes multiple voice streams into the final audio output.**

Routes each active remote participant's decoded audio frame to that participant's own audio sink; spatialization (distance attenuation / panning) is applied downstream by the audio graph. A second step feeds system audio into the frame for echo-cancellation purposes.

**Performance notes:** O(active voice streams). Fast per-stream.


---

### CustomAudioMixer::updateSources

**Updates voice source positions and states for spatial audio mixing.**


---

### AudioDeviceSharedState::OnPostMix / RobloxAudioDevice::OnPostMix / RobloxAudioDevice::OnMidMix

**Audio device callbacks — fire during the audio render pipeline for voice integration.**


---

### NeedMorePlayData / RecordedDataIsAvailable

**Audio device callbacks — signals that the playback device needs data or recording data is ready.**


---

## Recording

### RecordingDeviceManager::ProcessAndDeliverAudioBlock

**Processes a recorded audio block and delivers it to the voice pipeline.**


---

### RecordingDeviceManager::ReadRecordingBuffer

**Reads the recording ring buffer — extracts captured audio samples.**


---

## Transport

### StreamTransactionMessageTransport::receive / StreamTransactionMessageTransport::sendMessage

**Receives/sends voice transport messages over the voice data channel.**


---

## Statistics & Telemetry

### ClientVoiceRtcStatsEmitVoiceRtcStatsState::nextImpl

**Collects and emits voice statistics for voice quality monitoring.**


---

### ClientVoiceRtcStatsMonitorState::emitLegacyCombinedStats / emitSplitStats

**Emits voice chat quality statistics (packet loss, jitter, RTT) for telemetry.**


---

### ClientVoiceRtcStatsSubmitGetStatsAsyncState::nextImpl

**Submits an async request for voice stats from the peer connection.**


---

## VoiceChat Lifecycle

### VoiceChatInternal::VoiceChatInternal()

**VoiceChat system construction and initialization.**


---

### VoiceChatInternal::connectToSoundService / connectToVoiceChatService / disconnectFromVoiceChatService

**Connects/disconnects from the sound system and voice chat backend services.**


---

### VoiceChatInternal::connectToVoiceChatService->onJoined / onLeft / onPublishBegan / onPublishEnded / onSubscribeBegan / onSubscribeEnded

**Voice chat event handlers — triggered when participants join, leave, start/stop publishing, or subscribe/unsubscribe.**


---

### VoiceChatInternal::SoundVoice / SoundVoice->onApiEnrollmentChanged

**Voice service lifecycle handling — wires up (or tears down) VoiceChatService when it is added to or removed from the game; the paired handler tracks voice-API enrollment changes.**


---

## Device Management

### VoiceChatInternal::::DeviceRegistry::add / change / remove / getDevicesFor

**Manages audio input/output device registry — adds, changes, removes, and queries devices.**


---

### VoiceChatInternal::::DeviceRegistry::add->propertyChanged

**Handles an AudioDeviceInput Player-property change — re-maps which player owns a voice input device.**


---

### VoiceChatInternal::createAudioSink / createSystemAudioSinkOutput / createUnboundAudioSink / deleteSystemAudioSinkOutput

**Creates and destroys voice audio sinks. `createAudioSink` / `createUnboundAudioSink` produce per-participant output nodes; `createSystemAudioSinkOutput` / `deleteSystemAudioSinkOutput` attach/detach the system-audio ring buffer used for acoustic echo cancellation (AEC).**


---

### VoiceChatInternal::getAudibilityOf / getAudioDevice / getAudioProcessingSettings / getAndClearCallFailureMessage()

**Queries voice system state — audibility checks, device info, processing settings, error messages.**


---

## Voice System Details

### VoiceChatInternal Queries

- `getChannelId()` / `getGroupId()` / `getSessionId()` / `getSessionState()` — queries voice session state
- `getDm` / `getExposureStats()` / `getInputAudioSink` / `getMicDevices` — queries voice system state
- `getOutputCaptureAudioSink` / `getParticipants()` / `getVoiceExperienceId` — participant and experience info
- `getVoiceExperienceIdFromLua()` / `getVoiceChatApiVersion()` / `getVoiceChatAvailable()` — API queries from Lua
- `isContextVoiceEnabledLua()` / `isPublishPaused()` / `isSubscribePaused()` — state queries
- `isVoiceEnabledForUserIdAsync()` / `isVoiceEnabledForUserIdAsync()->successFunction` — async permission check
- `joinByGroupIdToken()` — joins voice by group token
- `publishPause()` / `publishPauseFromLua()` / `subscribePause()` / `subscribePauseAll()` — pause operations
- `VoiceChatInternal::subscribeBlock` / `VoiceChatInternal::subscribeRetry` / `VoiceChatInternal::subscribeUnblock` — subscription management
- `VoiceChatInternal::resetExposureStats()` / `VoiceChatInternal::synchronizeCallState` / `VoiceChatInternal::toPlayerActivityValueTable()` — utility operations
- `VoiceChatInternal::setMicDevice` / `VoiceChatInternal::setupDeviceRegistry` / `VoiceChatInternal::setupDeviceRegistry->callback` — device configuration
- `~VoiceChatInternal()` — destructor cleanup


---

### ClientVoiceRtcStatsMonitorState::emitSplitStats

**Emits split (per-stream) voice quality statistics.**


<br>
<br>

---

<a name="group-video"></a>
# 📄 `video.md` — Video Group

<sub>[↑ Contents](#contents)</sub>

The Video group contains scopes for video playback and processing: codec decode/encode, stream handling, format conversion, and video-frame rendering. These scopes run on dedicated media threads and occasionally on the main thread.

---

## Codec Operations (Decode & Encode)

### MediaCodecDecoder::receiveFrame / MediaCodecDecoder::sendPacket

**Android MediaCodec hardware decoder — sends encoded packets and receives decoded frames.**


---

### AlphaVideoMediaCodecDecoder::receiveFrame / AlphaVideoMediaCodecDecoder::sendPacket

**Android alpha-channel video decoder (transparent video).**


---

### MediaFoundationCodec::receiveFrameInternal / sendFrameInternal / receivePacketInternal / sendPacketInternal

**Windows Media Foundation codec — hardware-accelerated encode/decode on Windows.**


---

### MvtDecoder::receiveFrameInternal / sendPacketInternal / releaseFrame / decoderCallback / MvtEncoder::encoderCallback / receivePacketInternal / sendFrame

**MVT (Apple VideoToolbox) codec — Apple hardware encode/decode.**


---

### PS4VideoDec2Decoder::receiveFrameInternal / sendPacketInternal / releaseFrame

**PlayStation 4 hardware video decoder.**


---

### Av1SoftwareCodec::receiveFrameInternal / sendPacketInternal

**AV1 software codec — CPU-based AV1 decode.**

**Performance notes:** Software decoding is CPU-intensive. Used when hardware decode isn't available.


---

### OpusSoftwareCodec::receiveFrameInternal / receivePacketInternal

**Opus audio codec — encodes and decodes Opus audio for voice/audio streaming.**


---

### MediaCodecEncoder::receivePacket / MediaCodecEncoder::sendFrame

**Android hardware video encoder — feeds video frames to the platform encoder and dequeues the encoded packets.**


---

### McaEncoder::receivePacketInternal / McaEncoder::sendFrame

**Apple/Mac AAC audio encoder (Core Audio) — encodes PCM audio into AAC packets and dequeues them. (This is audio, even though it appears under the `Video` profiler group.)**


---

### VorbisSoftwareCodec::receivePacketInternal

**Vorbis software audio codec — encodes PCM audio into Vorbis packets (encoder output step).**


---

### VpxSoftwareCodec::receiveFrameInternal / sendFrameInternal / sendPacketInternal

**VP8/VP9 software codec — CPU-based VP8/VP9 encode/decode.**


---

### WinCodecDriver::receiveFrameInternal / receivePacketInternal / sendFrameInternal / sendPacketInternal

**Windows codec driver — dispatches encode/decode to the platform codec.**


---

### WebmInputFormat::readPacket

**Reads packets from a WebM container (demuxing).**


---

### Codec sub-operations

Lower-level codec I/O shared across the codecs above:
- `copyFromPacket` / `copyFromPacketAlpha` / `copyToFrame` / `copyToFrameAlpha` — packet/frame data copy
- `decodeAudio` / `decodeVideo` — decode entry points
- `vpx_buffer_copy` / `vpx_codec_encode` — VP8/VP9 decoded-frame buffer copy (decode path) and encode operation


---

## Codec Wrapper (RvCodec)

### RvCodecReceive / RvCodecSend

**High-level codec receive/send — wraps the platform-specific codec for the Rv (Roblox Video) pipeline.**

Receives decoded frames from the codec or sends raw frames to the encoder.

**Performance notes:** One call per video frame. Cost depends on the underlying codec (hardware = fast, software = slow).


---

## Playback & Stream

### VideoFeatureControl::save

**Saves video feature control settings.**


---

### VideoManager::incrementCounter

**Increments video system counters for telemetry.**


---

### VideoStream::renderStep

**Per-frame video stream update — delivers decoded frames for rendering.**


---

### convertToPng

**Converts a video frame to PNG format (for screenshots/thumbnails).**


---

### processPendingRequests / step

**Processes pending video requests and advances the video system state.**


---

### loadVideoCallback / onAssetLoaded / onReadAudio / readPacket

**Video asset loading and packet reading during playback.**


---

## Video Support Utilities

### RVScale::rvsScale

**Video frame scaling — resizes frames for different resolution targets.**


---

### RvSchedulerService::runCallbacks

**Dispatches scheduled video callbacks to a thread pool for asynchronous execution.**


---

### ScreenDensity::getCurrentScreenDensity

**Queries the current screen density for appropriate resolution selection.**


---

### CanvasGroupHandle

**Handles CanvasGroup video texture rendering.**


---

## Video Frame Rendering

Turning decoded video frames into on-screen textures (distinct from the 3D scene Render group).

### VideoManager

- `VideoManager::convertNV12FrameToR8Texture` / `VideoManager::convertNV12FrameToR8Texture:YuvToNv12` / `VideoManager::convertYUVFrameToR8Texture` / `VideoManager::processVideoSourceTexture` — video frame format conversion operations


---

### Frame output operations

- `bindHardwareBufferToVideoTexture` — binds hardware buffer for zero-copy video
- `doRenderVideo` / `flushRenderBuffers` / `renderFrameWithExternalBuffer` / `renderImage` — video rendering operations
- `libyuv::H420ToABGR` / `libyuv::I420AlphaToARGB` / `libyuv::NV12ToABGR` — color space conversion (libyuv)
- `getVisibilityIsViewUnBlocked` / `getVisibilityStatus` — visibility checks for video surfaces


<br>
<br>

---

<a name="group-player"></a>
# 📄 `player.md` — Player Group

<sub>[↑ Contents](#contents)</sub>

The Player group contains scopes for player character loading: rig construction, appearance application, avatar asset loading, and script installation. These scopes fire when a player joins or respawns.

---

## Character Loading Pipeline

### loadCharacterRig

**Loads the character rig (R6 or R15 skeleton) — creates the body part hierarchy and Motor6D joints.**

Constructs the base character model with all body parts, HumanoidRootPart, and joint connections. This is the structural foundation before appearance is applied.

**Performance notes:** One-time cost per spawn. Typically fast (structural assembly from templates).


---

### loadCharacterModel

**Assembles the base character model — loads the rig, sets up the Humanoid, and installs character scripts.**

Loads the character rig (via loadCharacterRig), replaces/initializes the Humanoid, and installs character scripts. It does not apply mesh appearance, body colors, or accessories — those are handled separately by applyCharacterAppearanceAssets.


---

### loadCharacterAppearanceAssets / applyCharacterAppearanceAssets

**Loads and applies appearance assets (clothing, body parts, face) to the character.**

Downloads and applies all avatar appearance items: shirts, pants, body colors, faces, and custom body parts.

**Performance notes:** Can be slow if many assets need downloading. First spawn is slower due to asset cache misses.


---

### applyAppearanceAsset

**Applies a single appearance asset to the character (one clothing item or body part).**


---

### loadCharacterScripts / loadHumanoidAnimationScripts

**Installs the Animate script and other character scripts onto the spawned character.**

Adds the default Animate script (controls idle, walk, run, jump animations) and Health script to the character.

**Performance notes:** Negligible.


---

### destroyMotor6Ds

**Destroys Motor6D joints during character teardown or rig swap.**


---

### handleAvatarFetchResponse

**Processes the avatar fetch API response — parses appearance data from the backend.**

Handles the HTTP response containing the player's avatar configuration (equipped items, body colors, etc.).


---

## Player Management

### ParentPlayer

**Parents a Player instance into the Players service — the core player installation step.**

Called when a player's connection is accepted. Parents the already-created Player instance into the Players service, making the player visible to the game.

**Performance notes:** Triggers cascading work (scripts fire PlayerAdded, character loading begins).


---

### RemovePlayer

**Removes a Player instance from the game — handles disconnection cleanup.**

Called on disconnect. Un-parents the Player instance, which triggers cascading cleanup of the character, backpack, and associated state.

**Performance notes:** Can trigger cascading cleanup (scripts fire PlayerRemoving, character destroyed).


<br>
<br>

---

<a name="group-slim"></a>
# 📄 `slim.md` — Slim Group

<sub>[↑ Contents](#contents)</sub>

The Slim group contains scopes for the SLIM (Scalable Lightweight Interactive Models) LOD system: a multi-stage geometry processing LOD pipeline for avatar and mesh geometry rendering at scale.

---

## SlimController (Legacy)

### SlimController::update

**Main update tick for the SLIM controller — orchestrates all SLIM work for the frame to update all models.**

Coordinates LOD transitions, streaming decisions, and binding updates for all SLIM models.

**Performance notes:** Cost proportional to the number of SLIM models in the scene.


---

### SlimController::updateBindings

**Updates render bindings for SLIM models — connects SLIM data to the rendering system.**


---

### SlimController::updateLoadRequests

**Processes load requests for SLIM model data (mesh, animation, texture streaming).**


---

### SlimController::makeStreamingDecisions

**Determines which SLIM model LOD levels to stream in/out based on distance and budget.**

Applies the streaming policy: closer models get higher LOD, distant ones are simplified or evicted.

**Performance notes:** O(SLIM models). Budget-aware — may evict data to stay within memory budget.


---

### SlimController::garbageCollect

**Garbage-collects SLIM data that is no longer needed (model left view, LOD downgraded).**


---

### SlimController::removeBindings

**Removes render bindings for SLIM models that are being unloaded.**


---

## SlimController2 (Current)

### SlimController2::update

**Main update for the current SLIM controller implementation.**


---

### SlimController2::processCommands

**Processes queued commands for SLIM models (load, unload, transition LOD).**


---

### SlimController2::updateLodStates

**Updates LOD states for all SLIM models — determines current LOD level based on distance/budget.**


---

### SlimController2::updatePrepareRenderBindings

**Prepares render bindings for the rendering system — sets up GPU data for SLIM models.**


---

## Load Balancer

### SlimLoadBalancerImpl::prepare

**Prepares load balancing data — refreshes configuration and processes pending load responses for the frame.**


---

### SlimLoadBalancerImpl::computeScores

**Computes priority scores for each SLIM model — determines who gets resources.**

Scores are based on recent visibility (time since last on-screen), on-screen size, and current LOD state.


---

### SlimLoadBalancerImpl::computeLodDownBias

**Computes a bias toward lower LODs when memory/performance pressure is high.**


---

### SlimLoadBalancerImpl::makeOptimizationScores / sortOptimizationScores

**Generates and sorts optimization scores for load-shedding decisions.**


---

### SlimLoadBalancerImpl::balanceLoadsAndEvictions

**Balances loading new model data against evicting low-priority data to stay within budget.**

The core resource management decision: what to load and what to evict.


---

### SlimLoadBalancerImpl::applyOptimization

**Applies load balancing decisions — triggers actual load/evict operations.**


---

### SlimLoadBalancerImpl::processLoadResponses

**Processes completed load operations — finalizes model data that finished loading.**


---

## Loader

### SlimLoaderImpl::update

**Updates the SLIM loader — processes the load queue and manages worker threads.**


---

### SlimLoaderImpl::workerStep

**One worker thread step — processes the SLIM load pipeline: content-load, ACR, manifest, chunk, and load requests.**


---

### SlimLoaderImpl::loadSynchronousChunks

**Loads SLIM data chunks that must be synchronous (blocking) — used for immediate-need models.**


---

## Renderer

### SlimRenderer::updatePrepare

**Prepares SLIM rendering data for the current frame — updates skinned bone data from replicated SLIM animation streams.**


---

### SlimRenderer::updatePrepareAnimations

**Processes the SLIM animation-stream deletion queue — tears down animation streams for models that no longer need them.**

**Performance notes:** Cost proportional to the number of SLIM animation streams being destroyed this frame.


---

## Replication

### SlimReplicationService::onHeartbeatClient / onHeartbeatServer

**Per-frame replication update for SLIM data — sends/receives SLIM state changes over the network.**

Client side receives SLIM data updates; server side sends them to relevant clients.


<br>
<br>

---

<a name="group-lc"></a>
# 📄 `lc.md` — LC Group (Layered Clothing / Cage Deformers)

<sub>[↑ Contents](#contents)</sub>

The LC group contains scopes for the layered clothing (LC) and cage deformer system: mesh deformation for clothing layers, body wraps, and avatar shape customization. These scopes fire on the main thread and worker threads during avatar rendering preparation.

---

## Core Deformation

### deformBase

**Builds RBF deformation solutions for WrapDeformer-equipped parts in the base layer — computes how each body part's cage transforms from its rest pose to the deformed pose.**

Runs the core deformation algorithm for each avatar body part, producing the deformation field that will be used to deform the avatar body mesh.

**Performance notes:** Cost per avatar with layered clothing. More layers = more deformation work.

**What game creators can do:**
- Limit the number of layered clothing items per character
- Use fewer NPCs with full layered clothing in close proximity
- Characters far from the camera may have deformation LOD applied


---

### deformFileMeshData

**Deforms a file mesh (asset mesh) using cage data — reshapes a clothing mesh to fit the body.**

Applies the cage deformation to a specific clothing mesh asset, warping it to conform to the avatar's body shape.


---

### wrapDeformers

**Processes all WrapDeformer instances — applies layered clothing deformations.**

Iterates all active wrap deformers and applies their deformation stack.


---

### solvePose / solver / fitting

**Runs the layered-clothing deformation for one avatar — from pose setup through fitting the clothing to the body.**

The numerical solver that determines how clothing deforms to fit the body without interpenetration.


---

### finalSolution

**Computes the final solution for the deformation — output mesh positions.**


---

## Layer Processing

### createLayers / layer

**Creates and processes individual clothing layers in the deformation stack.**

Each piece of layered clothing is a separate layer. Layers are processed from inside out.


---

### createPieces

**Creates deformation pieces from the clothing mesh — segments the mesh for independent deformation.**


---

### expand / expand layer / puffines

**Expands clothing layers outward from the body — adds thickness and prevents interpenetration.**

"Puffiness" applies volume to thin clothing layers.


---

### target

**Builds the deformation field for body target parts — maps the original cage to the compressed cage positions so the body mesh deforms inward under clothing.**


---

## Mesh Operations

### createAlternateMeshes

**Creates deformed mesh for fitted layered clothing.**


---

### compress / compressBody

**Geometrically compresses stacked clothing layers — fits lower cage layers to the deformed upper layers so layers don't interpenetrate.**


---

### writeFileMesh

**Serializes a deformed layered-clothing mesh into the file-mesh format so it can be written to the persistent cache and evicted from memory.**


---

### serializeEvictableMeshes

**Serializes mesh data that can be evicted from memory and reloaded later.**


---

## HSR (Cage System)

### applyHSR

**Applies the HSR (Hidden Surface Removal) pass — removes body-mesh surfaces fully hidden beneath clothing layers so they aren't rendered.**


---

## System Operations

### Dispatcher::tick

**Ticks the deformation job dispatcher — processes queued deformation work.**


---

### SceneUpdater::forceUpdateAllExistingDeformers

**Forces every existing layered-clothing deformer to re-run in one pass.**

Runs when the scene is (re)prepared for a render — for example a capture or thumbnail pass — rather than during normal per-frame play.

**Performance notes:** Cost scales with the number of layered-clothing avatars, since every deformer is refreshed at once.


---

### invalidate instances

**Invalidates deformation instances that need recalculation (body shape changed, clothing added).**


---

### forceSyncUpdate

**Forces a synchronous (blocking) deformation update — waits for results immediately.**

**Performance notes:** Can stall the frame if deformation is complex. Normally, deformation is async.


---

### rbxStorageSet

**Stores deformation cache data in RbxStorage for persistence.**


<br>
<br>

---

<a name="group-systems"></a>
# 📄 `systems.md` — Systems Group

<sub>[↑ Contents](#contents)</sub>

The Systems group contains scopes for system-level engine operations: async asset loading, dynamic flag reloading, storage operations, timer services, CPU frequency probing, and other infrastructure tasks that run as background jobs or on the main thread.

---

## Asset Loading

### Async_Mesh

**Background mesh asset loading — decodes and prepares mesh data from downloaded assets.**

Runs on a worker thread to build GPU-ready vertex/index data for part geometry — basic part shapes (blocks, spheres, cylinders, wedges, trusses), solid-modeling meshes (unions/negations), and MeshPart meshes.

**Performance notes:** Background work. Doesn't block the main thread but competes for CPU time.


---

### Async_Texture

**Background texture asset loading — decodes image data into GPU textures.**

Runs on a worker thread to decode image formats (PNG, JPEG, WebP) into GPU texture data.

**Performance notes:** Background work. Image decoding can be CPU-intensive for large textures.


---

## Configuration

### Dynamic Flag Reloader

**Reloads dynamic fast flags from the CDN — checks for updated flag values.**

Periodically fetches the latest dynamic flag configuration to apply live-tuning changes.

**Performance notes:** Network I/O. Runs infrequently (minutes between reloads).


---

### InstallDefaultScripts

**Initializes the default StarterPlayerScripts (PlayerModule: control, camera, and player-script loader) once at service startup.**

Loads the built-in scripts/PlayerScripts project into StarterPlayerScripts. Runs a single time, not per character spawn. The Animate and Health character scripts are installed elsewhere.

**Performance notes:** One-time setup cost at startup. Negligible.


---

## Storage

### RbxStorage::asyncWrite

**Asynchronously writes data to RbxStorage, the engine's internal storage backend.**

Background file I/O for internal engine storage.


---

### RbxStorage::cleanupThreadFunc

**Background cleanup thread for RbxStorage — removes expired entries.**


---

### RbxStorage::rollingTelemetry

**Emits rolling telemetry about storage usage and performance.**


---

### read_or_mmap

**Reads or memory-maps a file from storage.**


---

## Timer Service

### timerServiceOnHeartbeat

**Steps the timer service — fires any timers whose deadlines have passed.**

Processes the engine-internal timer queue, executing callbacks for expired timers.

**Performance notes:** O(expired timers). Usually fast.


---

### timerServiceDelay / timerServiceCancel

**Schedules or cancels a timer in the timer service.**


---

## Diagnostics

### ProbeCPUFrequency

**Measures CPU frequency — used for accurate timing calibration.**

Runs periodically to detect CPU frequency scaling that could affect profiler timing.


---

### notifyHeartbeatAlive

**Pings the hang detector to indicate the heartbeat is still running.**

Prevents the hang detector from triggering a false alarm by confirming the main thread is alive.


---

### print

**Scope for engine print/log output (when profiled).**


<br>
<br>

---

<a name="group-jobs"></a>
# 📄 `jobs.md` — Jobs Group (TaskScheduler)

<sub>[↑ Contents](#contents)</sub>

The Jobs group contains scopes for all TaskScheduler jobs. Each job appears as a profiler scope whenever it runs, and the scope covers the job's entire per-frame execution.

Jobs appear in the profiler with the format: **`JobName(Priority;ArbiterName)`** — e.g., `Simulation(Simulation;LuaApp)`.

---

## TaskScheduler Infrastructure

### TS::Step

**One step of the TaskScheduler — selects and dispatches the next ready job.**


---

### TS::JobStep

**Executes one job's step() function — the inner scope of actual job work.**


---

### TS::JobWait

**A job is waiting for a condition (blocked by tryJobAgain mechanism).**


---

### TS::ArbiterStep

**Steps the arbiter — manages job ordering within one DataModel context.**


---

### TS::Reschedule

**Reschedules a job that has more work remaining.**


---

### TS::GC

**TaskScheduler garbage collection — removes completed or dead jobs.**


---

### AsyncTask

**Executes an asynchronous task from a task queue.**


---

## Frame-Defining Jobs

### Heartbeat

**The per-frame heartbeat job — runs all heartbeat subscribers and RunService.Heartbeat callbacks.**


---

### Simulation

**The physics simulation job — steps humanoids, animation, physics solver at fixed rate.**

**Performance notes:** An umbrella for the fixed-rate step — its width is the sum of the physics solve, humanoid stepping, and animation it runs.

**What game creators can do:**
- Reduce the number of moving physics bodies and constraints, and let bodies sleep when at rest.
- Reduce the number of actively-simulated `Humanoid`s and simultaneously-playing animations.
- Expand the scope to see whether physics, humanoids, or animation dominates, then target that.


---

### RenderJob

**The main rendering job — executes the full render pipeline (Perform phase).**

**Performance notes:** An umbrella for the whole render pipeline — its width is the sum of its passes (scene render, geometry updates, post-effects, GPU waits).

**What game creators can do:** Expand it and target the widest child pass — e.g. the `Scene` render, `updateRenderQueue`, or a GPU wait (`waitOnGpu` / `waitUntilCompleted`, which indicate a GPU-bound frame). Each of those entries has its own levers.


---

### PreRenderJob

**The pre-render job — runs the render step ahead of the main render job (and updates VR state on VR platforms).**


---

### SimpleRenderStepJob

**A render-step job used in headless / minimal rendering contexts. Calls the same render step as the main render jobs.**


---

## Input

### InputDispatch

**Dispatches input events — processes keyboard, mouse, touch, and gamepad input.**


---

## Script

### LuaGc

**Runs the Luau garbage collector as a scheduled job.**


---

### WaitingHybridScriptsJob

**Resumes scripts waiting on `Instance:WaitForChild()` or `task.wait()`.**

Usually runs ~30 times per second. It has an execution-time budget, so a heavy backlog of waiting scripts is spread across multiple runs.

**What game creators can do:**
- Reduce the number of waiting scripts, or shorten long computations before a script yields


---

## Network

### Allocate Bandwidth and Run Senders

**Allocates network bandwidth budget and sends replication data to clients.**


---

### Replicator ProcessPackets

**Processes received network packets — deserializes incoming replication data.**

Processes contents of network packets, such as motion, event invocations and property changes.

**What game creators can do:**
- Reduce the number or size of objects being replicated, or do this in incremental steps. May increase if map size increases.


---

### Replicator SendData

**Sends queued instance and property data to connected clients.**


---

### Replicator StreamData

**Streams instances to/from clients based on proximity.**


---

### Replicator GC Job

**Manages instance-streaming region lifecycle on the client — pauses, expands, and garbage-collects streamed regions based on the character's position.**


---

### Replicator StatsSender

**Sends replicator statistics for diagnostics.**


---

### receive physics Mega Job

**Receives and applies physics state updates from the server.**


---

### Streaming Solver V2

**Computes which instances each client should have streamed in.**


---

### InstanceObjectManager Job

**Manages instance lifecycle for streaming — creates/destroys instances as they stream in/out.**


---

### Distributed Physics Ownership

**Determines whether the server or a client has authority over certain instances such as parts.**

**What game creators can do:**
- Reduce the amount of parts that frequently switch network ownership, especially those with common interaction


---

### Net PacketReceive

**Receives raw network packets from the transport layer.**

Receives network packets. If many objects or events are being replicated, this step takes longer.

**What game creators can do:**
- Replicate fewer objects or events


---

### Net Peer Send

**Sends raw network packets to connected peers.**


---

### Net Peer Stats

**Collects network peer statistics (latency, bandwidth, packet loss).**


---

### Network Join Processor

**Processes player join requests — builds the initial world snapshot.**


---

### Network Quality Processor / Network Quality Responder

**Periodic server-side per-connection maintenance jobs. Each runs over all client connections on a throttled interval (a few times per second).**


---

### Network Disconnect Clean Up

**Cleans up after a player disconnects.**


---

### ServerToClientSender

**Sends real-time text translations from the server to a specific client (server-side job).**


---

## Sound

### Sound / Sound Job Studio

**The runtime `Sound` job advances the audio mix each frame; `Sound Job Studio` is a Studio-only job that steps sound-asset loading into channels.**


---

## Navigation

### NavigationJob

**Processes navigation mesh generation on a background thread.**


---

### PathUpdateJob

**Updates computed paths when the navmesh changes.**


---

## Terrain

### GridJob

**The terrain worker job — processes terrain read/write/LOD operations.**


---

## Video

### Video

**Video playback job — steps VideoFrame playback each frame (VideoService clients, playback managers, and thumbnail generation).**


---

### VideoCaptureTimer

**Monitors elapsed time during video capture and stops capture when duration limit is reached or render buffer timeout occurs.**


---

### VideoStreamMonitor

**Monitors active video streams and closes a stream when no new frames have arrived for longer than a timeout (derived from the stream's frame rate).**


---

### Format Muxer

**Muxes audio/video streams into a container format.**


---

### LocalPlaybackJob

**Handles local video playback.**


---

## Data Services

### DataStoreJob

**Manages DataStore request execution including adding throttling budgets, executing throttled and retry requests, refetching cached keys, and refreshing locks.**


---

### MemoryStoreJob

**Processes MemoryStore requests.**


---

### PlayerDataJob

**Handles player-specific data operations.**


---

## Content & Assets

### ModelMesh

**Computes optimized mesh representations of 3D models for LOD rendering, processing changed models and managing async mesh generation.**


---

### ThumbnailFetchJob

**Fetches asset thumbnails.**


---

### BlockingRead / ReadAsync

**Synchronous and asynchronous terrain voxel-grid reads — read voxel channel data (material/volume) from the terrain grid via the VoxelJobManager.**


---

## Analytics & Services

### AnalyticsServiceJob

**Batches and sends analytics events.**


---

### CollectionServiceJob

**Collects per-frame CollectionService tag operation metrics, processes pending player disconnections for tag rule cleanup, and reports telemetry.**


---

### Romark Phase Tracking Job

**Tracks performance phases for telemetry.**


---

### ScreenTimeHeartbeatJob

**Executes a periodic heartbeat for screen time tracking.**


---

### DataModelSnapshotJob

**Captures DataModel state snapshots.**


---

### SoapMicroprofilingHeartbeatJob

**Profiling heartbeat for server diagnostics.**


---

## HTTP

### HttpRbxApiJob

**Processes HTTP API requests to Roblox services.**


---

## Logging & Diagnostics

### LogServiceJob

**Batches and processes log events for analytics/telemetry on a 1 Hz interval.**


---

### HangDetectionJob

**A per-frame heartbeat job that marks the main loop alive; a background watcher thread flags a hang if the heartbeat stops.**


---

### NotifyAliveJob

**Periodically signals that the main thread is responsive.**


---

### MemoryPrioritizationJob

**A periodic client-side job that monitors how much memory the app is using against what's available and tracks the current level of memory pressure. As memory runs low it asks memory-holding systems to release memory so the app stays within the device's memory budget. Runs on mobile and Windows clients, and matters most on memory-constrained devices such as phones and tablets.**

It ticks at a low fixed rate, so it is normally negligible in a capture. If it is doing noticeable work — or running its checks frequently — the client is under memory pressure (running low on available memory), which on constrained devices is what precedes out-of-memory problems.

**Performance notes:** Tiny under normal conditions. Elevated activity here is a *symptom* of high memory usage, not a frame-cost problem in the job itself.

**What game creators can do:**
- Reduce the client's memory footprint: fewer and smaller textures, meshes, and sounds; reuse assets; and keep resident instance counts down.
- Use instance streaming so distant content isn't all held in memory at once.
- Watch for memory leaks — memory that only ever grows (e.g. instances, connections, or tables never cleaned up).

Keeping memory usage low keeps pressure low, so the engine doesn't have to shed memory and is far less likely to run out of memory on constrained devices.


---

### TimerTickerJob

**Advances engine timer ticks.**


---

### PhysicsTrackerJob

**Collects physics state changes (position/rotation) for BaseParts that moved and batches them by frame intervals.**


---

## Configuration

### CreatorConfigProviderPollingJob / CreatorConfigProviderReportingJob / CreatorConfigPlatformProviderPollingJob / CreatorConfigPlatformProviderReportingJob

**Polls and reports creator configuration settings.**


---

## Platform-Specific

### PlayStationTerminateMsgDialogJob

**PlayStation platform — handles termination message dialogs.**


---

## Other

### RCCInstanceTrackingDMJob

**Server instance tracking.**


---

### PluginOTAJob

**Over-the-air plugin update checking.**


---

### ModifyResolveUnavailable

**Resolves unavailable terrain modification requests.**


---

### TextScraper::ScraperJob / TextScraper::ServiceScraperJob

**Text scraping jobs for automatic localization.**


---

### TotalPlayerCountJob_

**Tracks total player count for analytics services.**


---

### EventBroadcastrelayFireEventJob

**Fires queued events through the cross-DataModel event-relay system.**


---

## Task Queues

Task queues appear as jobs with "TaskQueue" in the name. They batch small operations.

### WorkspaceTaskQueue / ScriptContextTaskQueue / HumanoidParallelManagerTaskQueue / AnimatorParallelManagerTaskQueue / SlimReplicationTaskQueue / SmoothClusterTaskQueue / SceneUpdaterTaskQueue / DataModelCharacterTaskQueue

**Various task queues that batch operations for their respective subsystems.**


---

### MegaReplicatorTaskQueue / MegaReplicatorPPRTaskQueue

**Server-side replication task queues. `MegaReplicatorTaskQueue` runs the per-frame work of sending replicated state out to connected clients — property changes, physics and touch updates, instance streaming, and the initial snapshot sent to joining players; `MegaReplicatorPPRTaskQueue` processes incoming physics updates in parallel. Both run on the server, so a wide block here is server frame time, not the client's.**

The work is spread across worker threads and is fundamentally per-connected-player: for each client the server serializes that client's share of changed state. It gets wide when there is simply a lot to replicate. If it is wide, expand it — the widest child phase (sending property changes, sending physics/touch updates, instance streaming, or a joining player's snapshot) points to which cause below dominates.

**Performance notes:** Cost scales roughly with (connected players) × (amount of state changing per player each frame). Player joins add a bursty spike, since a large slice of the world is serialized for the joining client.

**What game creators can do:**
- Reduce how many properties change each frame on replicated instances — avoid rewriting properties every frame; batch or throttle updates, and change only what truly needs to replicate.
- Reduce the number of moving, network-simulated parts and unnecessary touch events; anchor parts that don't need to move.
- In large or dense worlds, lower `Workspace.StreamingTargetRadius` and reduce the total instance count so less is streamed to each player.
- Spread player joins out over time where possible — join bursts are the most expensive moments.
- Fewer simultaneously connected players is the single biggest lever, since every phase scales per player.


---

## Marshalled Jobs

### Write Marshalled / Read Marshalled / None Marshalled

**Thread-safe operations that are marshalled to the DataModel thread. Write requires write lock, Read requires read lock, None requires no lock.**


<br>
<br>

---

<a name="group-profiler-runtime"></a>
# 📄 `profiler-runtime.md` — Profiler & Runtime Scopes

<sub>[↑ Contents](#contents)</sub>

This file documents three groups of scopes: the **MicroProfiler's own overhead** (its data collection and, when open, UI rendering); the **Runtime** group (fiber scheduling — background/foreground fibers, sleeping, and switching between fibers); and the **LuaBridge** group (Luau-to-engine property and method access). The data-collection and fiber scopes appear in essentially every capture; the profiler-UI scopes appear only while the profiler UI is open, and LuaBridge scopes only when scripts read or call engine instances.

---

## MicroProfiler Group

The profiler's own overhead — its UI rendering and data collection.

### MicroProfileFlip

**The profiler's per-frame housekeeping — collects timing data and advances the frame counter.**

Always present. Its duration is the profiler's own overhead.

---

### Draw / DrawBarView / Draw Graph / Detailed View

**Renders the profiler's on-screen UI (bar view, graph view, detailed view).**

Only present when the profiler UI is visible. Closing the profiler UI eliminates these.

---

### ThreadLoop

**The profiler's data collection thread loop.**

---

### Clear / Accumulate

**Clears per-frame data and accumulates statistics across frames.**

---

### ContextSwitchSearch

**Searches for OS thread context switches for the profiler's context-switch visualization.**

---

### FrameCheck / CPUFreq

**FrameCheck flags profiler frames whose capture data is incomplete (a thread's log buffer wrapped mid-frame); CPUFreq samples the CPU clock speed each frame.**

---

### MpDataUpdate

**Processes completed frames into the profiler's live data view — updates timer/thread registrations and feeds frame timing data into the timeline buffer.**

---

### WebServerUpdate

**Updates the profiler web server (for remote viewing via browser).**

---

## Runtime Group (Fiber Lifecycle)

Cooperative fiber runtime — the low-level scheduling layer that TaskScheduler Jobs run on. Engine jobs (including script execution, physics, rendering) execute inside these fibers. `(BG)` = background-group fiber, `(FG)` = foreground-group fiber.

### Thread (BG) / Thread (FG)

**A fiber is actively executing.**

This covers both script coroutines and engine async work. (FG) = foreground group — short-lived work that runs to completion (e.g. a task). (BG) = background group — long-lived work that may run indefinitely (e.g. a generator, or a loop that waits and wakes periodically).

---

### Sleep

**A worker thread is idle — waiting for new work to be scheduled.**

---

### Sched

**The scheduler is between fibers — selecting the next fiber to resume.**

---

## LuaBridge Group

Fires when Luau scripts interact with Roblox instances (method calls, property access). These nest inside the `Script_*` scope of the calling script.

### $namecall

**A `:Method()` call on a Roblox instance.**

If this is wide, a method call on a Roblox instance is taking a long time. Hover over the scope to see labels showing the **method name** and **class name** (e.g., `FindFirstChild`, `Model`).

---

### $index

**A `.Property` or `[key]` read on a Roblox instance.**

Hover to see labels showing the **property/field name** (or **child name**) and **class name**. `$index` covers both property/field reads and lookups of a child instance by name.

---

### $newindex

**A `.Property = value` or `[key] = value` write on a Roblox instance.**

Hover to see labels showing the **property name** and **class name**.

---

### $call

**Executes a Roblox instance member function or yielding function that was already resolved.**

Unlike `$namecall` which includes method lookup, `$call` fires when calling a pre-resolved function closure. Hover to see the function name and class.


<br>
<br>

---

<a name="group-telemetry"></a>
# 📄 `telemetry.md` — Telemetry Group

<sub>[↑ Contents](#contents)</sub>

The telemetry group (labeled "telem" in code) contains scopes for gathering, aggregating, and transmitting performance telemetry data to Roblox's backend analytics systems. These scopes fire periodically (not every frame) and measure the overhead of the telemetry system itself.

---

## Gathering

### telem / perfdata_v2

**Gathers performance data v2 — collects per-frame timing samples from telemetry-enabled scopes.**

The main telemetry gathering pass. Reads accumulated timing data from telemetry-enabled scopes and packages them for transmission.

**Performance notes:** Runs periodically (not every frame). If it appears in a profiler capture, it's measuring the gather cost itself. Usually < 1ms.


---

### telem / perfdata_v2_sendphase

**Sends gathered performance data to the backend — the network transmission phase.**

Serializes and transmits the collected performance metrics to Roblox analytics endpoints.


---

### telem / prof_v2 / profileTelemetry_v2

**Aggregates performance-statistics telemetry — FPS and frame-time averages/min/max plus quality-level percentiles.**


---

## Category-Specific Gathering

### telem / gfx

**Gathers graphics/rendering telemetry — GPU/adapter info, display resolution, lighting counts, quality level, and render-pass stats.**


---

### telem / memory / memory_v2

**Gathers memory telemetry — heap usage, texture memory, mesh memory, Luau heap size.**


---

### telem / script

**Gathers script telemetry — Luau garbage-collection time (GC assist and step) and heap size, for the game and core-script VMs.**


---

### telem / avatar

**Gathers avatar memory telemetry — humanoid count, texture memory, and mesh memory usage.**


---

## Session Tracking

### telem / sessionTracking_v2 / sessionTracking_ph2

**Per-session telemetry aggregation — gathers and records the session's periodic perf-data and stability markers.**

Aggregates the session's performance data and records session markers used to determine whether a session ended cleanly.


---

## Transmission

### telem / send

**Sends a telemetry batch to the backend.**

**Performance notes:** May spike if many metrics are queued.


---

## Transport & Service Telemetry

### RbxTransport telemetry scopes

Transport-layer telemetry scopes (in the "RbxTransport" group):
- `BasePacketSender.processPendingTx`
- `LibuvEventLoop.runOnce`
- `QuicPacketReader.onRecv`
- `SysEventLoop.processAllEvents`

### SoundOutput telemetry scopes

Audio output telemetry:
- `renderAudio` — measures audio render callback duration

### LocalStorage / MemProfStorage telemetry scopes

Storage telemetry:
- `asyncFlushTask` — measures storage flush duration
- `mappedFileFlush` — measures memory-mapped file flush

---

### Counter::send / EphemeralCounter::send / EphemeralStat::send / Event::send / Stat::send

**Telemetry transmission operations — sends counters, ephemeral stats, events, and statistics to the analytics backend.**


<br>
<br>

---

<a name="group-misc"></a>
# 📄 `misc.md` — Additional Profiler Groups

<sub>[↑ Contents](#contents)</sub>

This file documents profiler groups that don't have their own dedicated file due to small scope count.

---

## Skeleton Group

Animation skeleton operations.

### SkeletonWatcher::createAndBindToSkeleton / SkeletonWatcher::create / SkeletonWatcher::bindToSkeleton

**Creates a skeleton watcher and binds it to a character's skeleton — monitors rig changes for animation invalidation.**


---

### allocatePose / getPosePrepare / fetchPhysics / fetchAnimation / buildSkeleton / getBoneIndicesBySortedBoneHashes

**Pose buffer management: allocates pose storage, prepares pose data, fetches physics/animation state, builds the skeleton structure, and maps bones by hash.**


---

## FaceAnimatorService Group

Face tracking and animation.

### A2cInference / V2cInference

**Audio-driven and video-driven face-animation inference — runs ML models to generate facial animation from audio or camera input.**


---

### completeSdkSetup_AudioOnly / completeSdkSetup_ForModelDelivery

**Completes face tracker SDK setup for audio-only or full model delivery modes.**


---

### initAsync_ModelDelivery_JIT / initAsync_ModelDelivery_SetupAndLaunchMDS

**Async initialization of the face tracking model delivery system — JIT setup and model download service launch.**


---

### FinalDMTask_ModelDelivery

**Final DataModel task for face tracking model delivery completion.**


---

## HSR Group

Hidden Surface Removal for layered clothing — precomputes which body-mesh surfaces are hidden beneath clothing so they can be skipped at render time.

### GenerateHSRData / generate HSR data

**Computes which mesh surfaces are occluded by cage geometry, using reference, cage, and parent mesh data to generate compression-optimized hidden-surface-removal metadata.**


---

### HSR data deserialize / HSR data serialization

**Serializes/deserializes HSR data for network transfer or disk caching.**


---

### generate WrapLayer HSR data / generate from inner cage HSR data / generate to outer cage HSR data

**Generates HSR data for specific deformation targets: wrap layers, inner cage, and outer cage.**


---

## Rbf Group

Mesh deformation for layered clothing.

### solutionBuild

**Builds the deformation solution for mesh fitting.**


---

### deduplication/masking

**Deduplicates control points and applies masking for interpolation.**


---

### distance_matrix / target_matrix

**Computes distance and target matrices for deformation interpolation.**


---

### factorize / solve_reuse_llt / llt_cache_hit

**Solver operations for the deformation system; cached for reuse across frames. solve_reuse_llt reuses the cached factorization. llt_cache_hit fires when the cache is valid.**


---

## Runtime Group

Engine runtime infrastructure.

### IO::Process / IO::Request

**I/O dispatcher: IO::Process runs a blocking call thread worker with thread monitoring disabled. IO::Request executes individual blocking call function requests in a background thread.**


---

### ProcessAppEvents / handleSDLMarshalledEvent

**Processes queued cross-thread work marshalled to the main thread. `handleSDLMarshalledEvent` runs a single marshalled callback delivered via the SDL event queue.** The same `ProcessAppEvents` step is also timed under the **App** group (see the App-group entry below); the group tag distinguishes the two.


---

### CallMessage / marshalledJob

**Platform-specific function marshalling — runs a callback on the thread it was marshalled to. On Android this is `CallMessage` (runs the callback on the main thread, with exception handling); some other platforms use `marshalledJob` instead.**


---

### parallelFor

**Generic parallel-for dispatch — distributes iterations across worker threads.**


---

## FroxelGrid Group

Light culling operations for the lighting system.

### cullLights / cullLightsCPU

**Culls lights against the froxel grid — determines which lights affect which screen regions. CPU variant for fallback path.**


---

### uploadGPULightCullData / uploadGPULightRenderData

**Uploads light culling data and rendering data to the GPU for the clustered lighting system.**


---

### Gather / Memcpy / WriteBack

**Gathers light data, copies to staging buffers, and writes back results from GPU.**


---

## Graphics Group

Low-level graphics API operations.

### Load font

**Loads a font into the graphics system.**


---

### commitChanges / commitCommandBuffer / makeCommandBuffer / queueSubmit

**GPU command buffer operations: creates command buffers, records commands, commits changes, and submits to the GPU queue.**


---

## RbxTransport Group

Network transport layer.

### BasePacketSender.processPendingTx / BasePacketSenderDeprecated.processPendingTx / PacketSenderDeprecated.processPendingTx

**Processes pending packet transmissions — sends queued network data.**


---

### ConnectionHandler.handle / ListenerHandler.handle

**Handles incoming connections and listener events at the transport layer.**


---

### QuicPacketReader.onRecv / LibuvEventLoop.runOnce / SysEventLoop.processAllEvents

**Transport I/O: reads QUIC packets, runs the libuv event loop, and processes system events.**


---

## ModelLOD Group

Model level-of-detail system.

### ModelLODComputation / GetGeometryForComputation / fetchModelMeshDependencies

**ModelLODComputation generates optimized mesh representations for model LOD rendering. GetGeometryForComputation collects geometry from CSG/MeshParts/BaseParts. fetchModelMeshDependencies async-loads all mesh/texture dependencies.**


---

## D3D11 Group

Direct3D 11 operations.

### commitChanges / new Texture

**D3D11-specific GPU operations: commits pending state changes and creates texture resources.**


---

## Ads Group

In-experience advertising.

### AdGuiHeartbeat / VisibilityHeartbeat

**Per-frame ad system heartbeats — updates ad GUI state and checks ad visibility.**


---

### AdPortalValidate / getPerfData

**Validates ad portal state and collects performance data for ad telemetry.**


---

### Raycasting / VisibilityCheck

**Ad visibility checks: `Raycasting` casts a ray to test whether the ad surface is occluded, while `VisibilityCheck` evaluates screen-space area coverage and tracks viewability history.**


---

## App Group

Application lifecycle and thread marshalling.

### ProcessAppEvents / UpdateAppState / WaitForEvents

**Application event loop: processes queued events, updates application state, and waits for new events.** Its `ProcessAppEvents` step is the same main-thread event processing documented under the **Runtime** group — the label appears in both groups.

---

### App / Submit

**Submits a marshalled function call from a background thread to the main thread.**

---

### App / Execute

**Executes a marshalled function call on the main thread.**

---

### App / ProcessMessages

**Processes all pending marshalled messages on the main thread.**

**Performance notes:** If wide, many background threads are queuing work for the main thread.

---

### App / ProcessMessagesOfType

**Processes messages of a specific type/priority.**

---

### App / OnAsyncEvent (Windows)

**Runs a function marshalled onto the main thread — the handler for an async Windows event.**

---

## Camera Group

### StepEngineCamera / StepSubject

**Steps the engine camera system and advances the camera subject (target tracking).**


---

## CageDeformer Group

### Initialize Scratch / deformVertices

**Initialize Scratch creates buffers for vertex normals, positions, and validity tracking. deformVertices applies cage deformation to mesh vertices using precomputed solutions.**


---

## SmoothCluster Group

### HashSetInsert / onTerrainRegionChanged

**Hash set insertion for terrain chunk tracking and terrain region change notifications.**

---

## AssetProvider Group

Asset loading pipeline operations.

### AssetProvider / AssetProviderWorkflowExecutorCycle

**One cycle of the asset provider workflow executor — processes asset loading steps.**

---

### AssetProvider / onWorkflowComplete

**Offloads asset workflow completion telemetry outside the hot loop.**

---

## CSG Group

Constructive Solid Geometry operations (Union, Intersect, Subtract).

### CSG / csgFunc

**Executes a CSG boolean operation (union, intersect, subtract) on meshes.**

Runs asynchronously when players or scripts use CSG operations.

**Performance notes:** CSG operations are expensive. Complex meshes with many faces take longer.

**What game creators can do:**
- Avoid runtime CSG operations in performance-critical paths
- Pre-compute CSG at edit time rather than runtime
- Use simpler meshes for CSG operands

---

### CSG / commit

**Commits CSG results — finalizes the computed mesh and updates the PartOperation.**

---

### CSG / getInstancesThatNeedFetching

**Categorizes CSG operand instances into those needing validation, asset fetching, or rebuilding.**

---

### CSG / collision geometry / Generate physics data / dcd convert

**Generates physics collision geometry from CSG results.**

---

### CSG / planesForVertex

**Collects the BSP planes that pass through a given vertex during CSG computation.**

---

## Terrain Group

Terrain editing operations (distinct from Voxel group which handles voxel storage).

### Terrain / castRay

**Casts a ray against terrain geometry.**

---

### Terrain / generateTilesWorker / generateTile / SDFFromHeightMap

**Generates terrain tiles from heightmap data.**

Used by terrain import tools.

---

### Terrain / importHeightmapSetup / drainImportQueue

**Sets up and processes heightmap import operations.**

---

## Micro-Groups

These groups have only 1-2 scopes each. The header shows the profiler group name.

### GameplayNet

- `UpdatePlayerLocations` — updates player spatial locations for gameplay networking
- `updateNOUOwners` — updates Network Ownership Unit owners

### RakNet

- `RakPeer::ProcessNetworkPacket` — processes buffered incoming transport-layer packets
- `RakPeer::HandleActiveSystemList` — manages the list of active peer connections

### LightGrid

- `gatherLights` — gathers the visible lights for the light-grid update

### GizmoManager

- `collectGizmos` — gathers the active constraint/attachment visualization gizmos to draw
- `preprocessBudgetedGizmos` — prepares those gizmos for drawing; capped per frame — when more constraints are visible than the budget allows, the closest ones are prioritized by distance

### VertexNormals

- `initializeAndResize` — initializes and resizes vertex normal computation buffers
- `solveSmoothingGroups` — computes smoothing groups for vertex normal averaging

### LuauExpression

- `LuauExpression::evaluate` — evaluates a Luau expression
- `LuauExpression::parse` — parses a Luau expression string

### ScriptContext

- `WaitForLuauGcThread` — waits for the Luau GC thread to finish before proceeding

### Humanoid

- `getContactRepelForceInBalancing` — computes repulsion force to prevent humanoid overlap

### CaptureService

- `onVideoCaptureContentReadyAsyncTask` — background task that runs after a `CaptureService` video recording finishes; reads the recorded video file to prepare it for the capture-ready flow (`CaptureService:StartVideoCaptureAsync`)

### TaskQueue

- `addTasks` — adds multiple tasks to a task queue

### Content

- `processTask` — processes a content loading task

### LOD

- `updateLODLevels` — updates script-level-of-detail levels for script instances (not model geometry LOD)

### Texture

- `GenerateComponents` — generates texture component data

### FileMeshData

- `computeUniformBoxmap` — computes uniform box-map texture coordinates for a mesh
- `ReadFileMesh` — reads and decodes a mesh file asset

### VoiceControlPlane

- Voice chat control plane operations

### SoundOutput

- `renderAudio` — audio render output callback

### MemProfStorage

- `mappedFileFlush` — flushes memory profiler storage

### LocalStorage

- `asyncFlushTask` — async local storage flush

### System

- System-level operation

### Compress

- Data compression operation

### CodeGen

- Luau native code generation

### Copy

- Copy operation

### Debug

- Debug operation

### (OverflowGroup)

- `(OverflowTimer)` — overflow bucket for CPU engine scopes when the profiler runs out of available scope slots; the real engine scope name is not recorded

### (OverflowGroupGpu)

- `(OverflowTimerGpu)` — overflow bucket for GPU engine scopes when the profiler runs out of available scope slots; the real engine scope name is not recorded
