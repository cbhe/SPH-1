/*
The MIT License (MIT)

Copyright (c) 2014 Adam Simpson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>

#include "mpi.h"
#include "hash_sort.h"
#include "renderer.h"
#include "setup.h"
#include "fluid.h"
#include "communication.h"

#ifdef BLINK1
#include "blink1_light.h"
#endif

int main(int argc, char *argv[])
{
    // Initialize MPI
    MPI_Init(&argc, &argv);
    int rank;

    // Rank in world space
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    create_communicators();

    create_MPI_types();

    // Rank 0 is the render node, otherwise a simulation node
    if(rank == 0)
        start_renderer();
    else
        start_simulation();

    free_MPI_types();
    MPI_Finalize();
    return 0;
}

void start_simulation()
{
    unsigned int i;

    int rank, nprocs;

    MPI_Comm_rank(MPI_COMM_COMPUTE, &rank);
    MPI_Comm_size(MPI_COMM_COMPUTE, &nprocs);

    printf("compute rank: %d, num compute procs: %d \n",rank, nprocs);

    // Struct to hold all simulation values
    fluid_sim_t fluid_sim;

    // Allocate structs used in simulation
    alloc_sim_structs(&fluid_sim);

    // Initialize simulation parameters
    // Additionally set fluid and  boundaries
    // Requires MPI_Bcast to send screen aspect ratio
    init_params(&fluid_sim);

    // Partition problem, allocate memory, and initialize particles
    // Requires MPI_Send for particle counts
    alloc_and_init_sim(&fluid_sim);

    // Send initial parameters to render node and initialize light
    // Requires MPI_Gatherv
    sync_initial_params(&fluid_sim);

    // Initialize RGB Light if present
    #if defined BLINK1
    rgb_light_t light_state;
    float *colors_by_rank = malloc(3*nprocs*sizeof(float));
    MPI_Bcast(colors_by_rank, 3*nprocs, MPI_FLOAT, 0, MPI_COMM_WORLD);
    init_rgb_light(&light_state, 255*colors_by_rank[3*rank], 255*colors_by_rank[3*rank+1], 255*colors_by_rank[3*rank+2]);
    free(colors_by_rank);
    // Without this pause the lights can sometimes change color too quickly the first time step
    sleep(1);
    #endif    

    // Setup dummy values for MPI
    float *null_float = NULL;

    MPI_Request coords_req = MPI_REQUEST_NULL;

    tunable_parameters_t *null_tunable_param = NULL;
    int *null_recvcnts = NULL;
    int *null_displs = NULL;

    int sub_step = 0; // substep range from 0 to < steps_per_frame

    // Main simulation loop
    while(1) {

        // Initialize velocities
        apply_gravity(&fluid_sim);

        // Advance to predicted position and set OOB particles
        predict_positions(&fluid_sim);

        // Make sure that async send to render node is complete
        if(sub_step == 0)
        {
            if(coords_req != MPI_REQUEST_NULL)
	        MPI_Wait(&coords_req, MPI_STATUS_IGNORE);
        }

        #if defined BLINK1
        char previously_active = params.tunable_params.active;
        #endif

        // Receive updated paramaters from render nodes
        if(sub_step == fluid_sim.params->steps_per_frame-1)
            MPI_Scatterv(null_tunable_param, 0, null_displs, TunableParamtype, &fluid_sim.params->tunable_params, 1, TunableParamtype, 0,  MPI_COMM_WORLD);

        #if defined BLINK1
        // If recently added to computation turn light to light state color
        // If recently taken out of computation turn light to white
        char currently_active = fluid_sim.params->tunable_params.active;
        if (!previously_active && currently_active)
            rgb_light_reset(&light_state);
        else if (!currently_active && previously_active)
            rgb_light_white(&light_state);
        #endif

        if(fluid_sim.params->tunable_params.kill_sim)
            break;

        // Identify out of bounds particles and send them to appropriate rank
        identify_oob_particles(&fluid_sim);

         // Exchange halo particles
        start_halo_exchange(&fluid_sim);
        finish_halo_exchange(&fluid_sim);

        // Hash particles, sort, fill particle neighbors
        find_all_neighbors(&fluid_sim);

        int solve_iterations = 4;
        int si;
        for(si=0; si<solve_iterations; si++)
        {
            compute_densities(&fluid_sim);

            calculate_lambda(&fluid_sim);
            // Generally not needed it appears but included for correctness of parallel algorithm
            update_halo_lambdas(&fluid_sim);

            update_dp(&fluid_sim);

            update_dp_positions(&fluid_sim);
            // Generally not needed it appears but included for correctness of parallel algorithm
            update_halo_positions(&fluid_sim);
        }

        // update velocity
        updateVelocities(&fluid_sim);

//        vorticity_confinement(fluid_particle_pointers, neighbors, &params);

        XSPH_viscosity(&fluid_sim);

        // Update position
        update_positions(&fluid_sim);

        // Pack fluid particle coordinates
        // This sends results as short in pixel coordinates
        if(sub_step == fluid_sim.params->steps_per_frame-1)
        {
            for(i=0; i<fluid_sim.params->number_fluid_particles_local; i++) {
                p = fluid_sim.fluid_particle_pointers[i];
                fluid_sim.fluid_particle_coords[i*2] = (2.0f*p->x/fluid_sim.boundary_global->max_x - 1.0f) * SHRT_MAX; // convert to short using full range
                fluid_sim.fluid_particle_coords[(i*2)+1] = (2.0f*p->y/fluid_sim.boundary_global->max_y - 1.0f) * SHRT_MAX; // convert to short using full range
            }
            // Async send fluid particle coordinates to render node
            MPI_Isend(fluid_sim.fluid_particle_coords, 2*fluid_sim.params->number_fluid_particles_local, MPI_SHORT, 0, 17, MPI_COMM_WORLD, &coords_req);
        }

        if(sub_step == fluid_sim.params->steps_per_frame-1)
            sub_step = 0;
        else
	    sub_step++;

    }

    #if defined BLINK1
        shutdown_rgb_light(&light_state);
    #endif

    // Free main sim memory
    free_sim_memory(&fluid_sim);

    // Cleanup structs
    free_sim_structs(&fluid_sim);
}

// Smoothing kernels

// (h^2 - r^2)^3 normalized in 2D
float W(float r, float h)
{
    if(r > h)
        return 0.0f;

    float C = 4.0f/(M_PI*h*h*h*h*h*h*h*h);
    float W = C*(h*h-r*r)*(h*h-r*r)*(h*h-r*r);
    return W;
}

// Gradient (h-r)^3 normalized in 2D
float del_W(float r, float h)
{
    if(r > h)
        return 0.0f;

    float eps  = 0.000001f;
    float coef = -30.0f/M_PI;
    float C = coef/(h*h*h*h*h * (r+eps));
    float del_W = C*(h-r)*(h-r);
    return del_W;
}

/*
void vorticity_confinement(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers; 
    neighbor_t *neighbors = fluid_sim->neighbor_grid->neighbors;
    param_t *params = fluid_sim->params;

    int i,j;
    fluid_particle_t *p, *q;
    neighbor_t *n;
    float epsilon = 20.01f;
    float dt = params->tunable_params.time_step;

    float x_diff, y_diff, vx_diff, vy_diff, r_mag, dw, dw_x, dw_y, part_vort_z, vort_z, eta_x, eta_y, eta_mag, N_x, N_y;

    for(i=0; i<params->number_fluid_particles_local; i++)
    {
        p = fluid_particle_pointers[i];
        n = &neighbors[i];

        vort_z = 0.0f;
        eta_x = 0.0f;
        eta_y = 0.0f;
        for(j=0; j<n->number_fluid_neighbors; j++)
        {
            q = n->fluid_neighbors[j];
            x_diff = p->x_star - q->x_star;
            y_diff = p->y_star - q->y_star;
            vx_diff = q->v_x - p->v_x;
            vy_diff = q->v_y - p->v_y;
            r_mag = sqrt(x_diff*x_diff + y_diff*y_diff);

            dw = del_W(r_mag, params->tunable_params.smoothing_radius);
            dw_x = dw*x_diff;
            dw_y = dw*y_diff;

            part_vort_z = vx_diff*dw_y - vy_diff*dw_x;
            vort_z += part_vort_z;

            if(x_diff<0.0000001 || y_diff<0.0000001)
                continue;

            eta_x += abs(part_vort_z)/x_diff;
            eta_y += abs(part_vort_z)/y_diff;            
        }

        eta_mag = sqrt(eta_x*eta_x + eta_y*eta_y);

        if(eta_mag<0.0000001)
            continue;

        N_x = eta_x / eta_mag;
        N_y = eta_y / eta_mag;

        p->v_x += epsilon*dt*N_y*vort_z;
        p->v_y -= epsilon*dt*N_x*vort_z;
    }
}
*/

void XSPH_viscosity(fluid_sim_t *fluid_sim)
{
    uint **fluid_particle_indices = fluid_sim->fluid_particle_indices;
    neighbor_t *neighbors = fluid_sim->neighbor_grid->neighbors;
    param_t *params = fluid_sim->params;

    int i,j;
    uint p_index, q_index;
    neighbor_t *n;
    float c = 0.1f;

    float x_diff, y_diff, vx_diff, vy_diff, r_mag, w;

    for(i=0; i<params->number_fluid_particles_local; i++)
    {
        p_index = fluid_particle_indices[i];
        n = &neighbors[i];

        float partial_sum_x = 0.0f;
        float partial_sum_y = 0.0f;
        for(j=0; j<n->number_fluid_neighbors; j++)
        {
            q = n->fluid_neighbors[j];
            x_diff = p->x_star - q->x_star;
            y_diff = p->y_star - q->y_star;
            vx_diff = q->v_x - p->v_x;
            vy_diff = q->v_y - p->v_y;
            r_mag = sqrt(x_diff*x_diff + y_diff*y_diff);
            w = W(r_mag, params->tunable_params.smoothing_radius);
            partial_sum_x += vx_diff * w;
            partial_sum_y += vy_diff * w;
        }
        partial_sum_x *= c;
        partial_sum_y *= c;
        p->v_x += partial_sum_x;
        p->v_y += partial_sum_y;
    }
}

void compute_densities(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    neighbor_t *neighbors = fluid_sim->neighbor_grid->neighbors;
    param_t *params = fluid_sim->params;

    int i,j;
    fluid_particle_t *p, *q;
    neighbor_t *n;

    for(i=0; i<params->number_fluid_particles_local; i++)
    {
        p = fluid_particle_pointers[i];
        n = &neighbors[i];

        p->density = 0.0f;

        // Own contribution to density
        calculate_density(p,p,params->tunable_params.smoothing_radius, params->particle_mass);
        // Neighbor contribution
        for(j=0; j<n->number_fluid_neighbors; j++)
        {
            q = n->fluid_neighbors[j];
            calculate_density(p, q, params->tunable_params.smoothing_radius, params->particle_mass);
        }
    }

}

void apply_gravity(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    param_t *params = fluid_sim->params;

    int i;
    fluid_particle_t *p;
    float dt = params->tunable_params.time_step;
    float g = -params->tunable_params.g;

    for(i=0; i<(params->number_fluid_particles_local); i++) {
        p = fluid_particle_pointers[i];
        p->v_y += g*dt;
     }
}

void update_dp_positions(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    param_t *params = fluid_sim->params;

    int i;
    fluid_particle_t *p;

    for(i=0; i<(params->number_fluid_particles_local); i++) {
        p = fluid_particle_pointers[i];
        p->x_star += p->dp_x;
        p->y_star += p->dp_y;

	// Enforce boundary conditions
        boundaryConditions(p, fluid_sim);
    }    
}

void update_positions(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    param_t *params = fluid_sim->params;

     int i;
     fluid_particle_t *p;

     for(i=0; i<(params->number_fluid_particles_local); i++) {
        p = fluid_particle_pointers[i];
        p->x = p->x_star;
        p->y = p->y_star;
    }    
}

void calculate_lambda(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    neighbor_t *neighbors = fluid_sim->neighbor_grid->neighbors;
    param_t *params = fluid_sim->params;

    int i,j;
    fluid_particle_t *p, *q;
    neighbor_t *n;

    for(i=0; i<params->number_fluid_particles_local; i++)
    { 
        p = fluid_particle_pointers[i];
        n = &neighbors[i];

        float Ci = p->density/params->tunable_params.rest_density - 1.0f;

        float sum_C, x_diff, y_diff, r_mag, grad, grad_x, grad_y;

        sum_C = 0.0f;
        grad_x = 0.0f;
        grad_y = 0.0f;
        // Add k = i contribution
        for(j=0; j<n->number_fluid_neighbors; j++)
        {
            q = n->fluid_neighbors[j];
            x_diff = p->x_star - q->x_star;
            y_diff = p->y_star - q->y_star;
            r_mag = sqrt(x_diff*x_diff + y_diff*y_diff);
            grad = del_W(r_mag, params->tunable_params.smoothing_radius);
            grad_x += grad*x_diff ;
            grad_y += grad*y_diff;
           }
           sum_C += grad_x*grad_x + grad_y*grad_y;

        // Add k =j contribution
        for(j=0; j<n->number_fluid_neighbors; j++)
        {
            q = n->fluid_neighbors[j];
            x_diff = p->x_star - q->x_star;
            y_diff = p->y_star - q->y_star;
            r_mag = sqrt(x_diff*x_diff + y_diff*y_diff);
            grad = del_W(r_mag, params->tunable_params.smoothing_radius);
            grad_x = grad*x_diff ;
            grad_y = grad*y_diff;
            sum_C += (grad_x*grad_x + grad_y*grad_y);
        }

        sum_C *= (1.0f/params->tunable_params.rest_density*params->tunable_params.rest_density);  

        float epsilon = 1.0f;
        p->lambda = -Ci/(sum_C + epsilon);
    }
}

void update_dp(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    neighbor_t *neighbors = fluid_sim->neighbor_grid->neighbors;
    param_t *params = fluid_sim->params;

    fluid_particle_t *p, *q;
    neighbor_t *n;
    float x_diff, y_diff, dp, r_mag;

    int i,j;
    for(i=0; i<params->number_fluid_particles_local; i++)
    {
        p = fluid_particle_pointers[i];
        n = &neighbors[i];

        float dp_x = 0.0f;
        float dp_y = 0.0f;
        float s_corr;
        float k = 0.1f;
        float dq = 0.3f*params->tunable_params.smoothing_radius;
        float Wdq = W(dq, params->tunable_params.smoothing_radius);

        for(j=0; j<n->number_fluid_neighbors; j++)
        {
            q = n->fluid_neighbors[j];
            x_diff = p->x_star - q->x_star;
            y_diff = p->y_star - q->y_star;
            r_mag = sqrt(x_diff*x_diff + y_diff*y_diff);
            s_corr = -k*(powf(W(r_mag, params->tunable_params.smoothing_radius)/Wdq, 4.0f));
            dp = (p->lambda + q->lambda + s_corr)*del_W(r_mag, params->tunable_params.smoothing_radius);
            dp_x += dp*x_diff;
            dp_y += dp*y_diff;
        }
        p->dp_x = dp_x/params->tunable_params.rest_density;
        p->dp_y = dp_y/params->tunable_params.rest_density;
    }   
}

// Identify out of bounds particles and send them to appropriate rank
void identify_oob_particles(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    oob_t *out_of_bounds = fluid_sim->out_of_bounds;
    param_t *params = fluid_sim->params;

    int i;
    fluid_particle_t *p;

    // Reset OOB numbers
    out_of_bounds->number_oob_particles_left = 0;
    out_of_bounds->number_oob_particles_right = 0;

    for(i=0; i<params->number_fluid_particles_local; i++) {
        p = fluid_particle_pointers[i];

        // Set OOB particle indices and update number
        if (p->x < params->tunable_params.node_start_x)
            out_of_bounds->oob_pointer_indices_left[out_of_bounds->number_oob_particles_left++] = i;
        else if (p->x > params->tunable_params.node_end_x)
            out_of_bounds->oob_pointer_indices_right[out_of_bounds->number_oob_particles_right++] = i;
    }
 
   // Transfer particles that have left the processor bounds
   transfer_OOB_particles(fluid_sim);
}


// Predict position
void predict_positions(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    param_t *params = fluid_sim->params;

    int i;
    fluid_particle_t *p;
    float dt = params->tunable_params.time_step;

    for(i=0; i<params->number_fluid_particles_local; i++) {
        p = fluid_particle_pointers[i];
	p->x_star = p->x  + (p->v_x * dt);
        p->y_star = p->y + (p->v_y * dt);

	// Enforce boundary conditions
        boundaryConditions(p, fluid_sim);
    }
}

// Calculate the density contribution of p on q and q on p
// r is passed in as this function is called in the hash which must also calculate r
void calculate_density(fluid_particle_t *p, fluid_particle_t *q, float h, float mass)
{
    float x_diff, y_diff, r_mag;
    x_diff = p->x_star - q->x_star;
    y_diff = p->y_star - q->y_star;
    r_mag = sqrt(x_diff*x_diff + y_diff*y_diff);
    if(r_mag <= h)
        p->density += mass*W(r_mag, h);
}

void checkVelocity(float *v_x, float *v_y)
{
    const float v_max = 20.0f;

    if(*v_x > v_max)
        *v_x = v_max;
    else if(*v_x < -v_max)
        *v_x = -v_max;
    if(*v_y > v_max)
        *v_y = v_max;
    else if(*v_y < -v_max)
        *v_y = -v_max;
}

void updateVelocity(fluid_particle_t *p, param_t *params)
{
    float dt = params->tunable_params.time_step;
    float v_x, v_y;

    v_x = (p->x_star-p->x)/dt;
    v_y = (p->y_star-p->y)/dt;

    checkVelocity(&v_x, &v_y);

    p->v_x = v_x;
    p->v_y = v_y;
}

// Update particle position and check boundary
void updateVelocities(fluid_sim_t *fluid_sim)
{
    fluid_particle_t **fluid_particle_pointers = fluid_sim->fluid_particle_pointers;
    param_t *params = fluid_sim->params;

    int i;
    fluid_particle_t *p;

    // Update local and halo particles, update halo so that XSPH visc. is correct
    for(i=0; i<params->number_fluid_particles_local + params->number_halo_particles; i++) {
        p = fluid_particle_pointers[i];
        updateVelocity(p, params);
    }
}

// Assume AABB with min point being axis origin
void boundaryConditions(fluid_particle_t *p, fluid_sim_t *fluid_sim)
{
    AABB_t *boundary = fluid_sim->boundary_global;
    param_t *params = fluid_sim->params;

    float center_x = params->tunable_params.mover_center_x;
    float center_y = params->tunable_params.mover_center_y;

    // Boundary condition for sphere mover
    // Sphere width == height
    float radius = params->tunable_params.mover_width*0.5f;
    float norm_x; 
    float norm_y;

    // Test if inside of circle
    float d;
    float d2 = (p->x_star - center_x)*(p->x_star - center_x) + (p->y_star - center_y)*(p->y_star - center_y);
    if(d2 <= radius*radius && d2 > 0.0f) {
        d = sqrt(d2);
        norm_x = (center_x-p->x_star)/d;
        norm_y = (center_y-p->y_star)/d;
	    
        // With no collision impulse we can handle penetration here
        float pen_dist = radius - d;
        p->x_star -= pen_dist * norm_x;
        p->y_star -= pen_dist * norm_y;
    }

    // Make sure object is not outside boundary
    // The particle must not be equal to boundary max or hash potentially won't pick it up
    // as the particle will in the 'next' after last bin
    if(p->x_star < boundary->min_x) {
        p->x_star = boundary->min_x;
    }
    else if(p->x_star > boundary->max_x){
        p->x_star = boundary->max_x-0.001f;
    }
    if(p->y_star <  boundary->min_y) {
        p->y_star = boundary->min_y;
    }
    else if(p->y_star > boundary->max_y){
        p->y_star = boundary->max_y-0.001f;
    }
}
