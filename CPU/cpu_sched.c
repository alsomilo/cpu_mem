#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <libvirt/libvirt.h>


#define MAX_DOMAIN_POSSIBLE  32
#define MAX_DOMAIN_NAME_LEN  32
#define MAX_VCPU_POSSIBLE    32
#define MYOS_GET_BITMASK(id)  (0x1 << id)

#define NANOSECOND           1000000000
#define USAGE_THRESHOLD      10

//#define DEBUG_ENA


typedef struct DomainInfo
{
  int          domain_id;
  char         *domain_name;
}DomainInfo_t;


typedef struct AllDomainsList 
{
  int          count;         /* number of domains in the array */
  virDomainPtr *domains; /* pointer to array of Libvirt domains */
  DomainInfo_t *domain_info;
}AllDomainsList_t;


typedef struct vCPUState
{
  int                    vcpu_id;
  int                    mapped_pcpu_id;
  unsigned long long int vcpus_time_ns;           /*vCPU time in nano-second*/
  double                 usage_perc;              /*vCPU usage, calculated by (vCPU time delta/ peroid in ns)*/
}vCPUState_t;

typedef struct DomainState
{
  int                    domain_id;
  char                   domain_name[MAX_DOMAIN_NAME_LEN];
  virDomainPtr           domain;                   /* Domain ptr for book keeping */
  int                    num_vcpus;                /* number of vCPUs in this domain*/
  vCPUState_t            vcpu_state_param[MAX_VCPU_POSSIBLE];
}DomainState_t;

typedef struct DomainListState 
{
  int                    num_domain;
  DomainState_t          domain_state_param[MAX_DOMAIN_POSSIBLE];

}DomainListState_t;



AllDomainsList_t myos_get_domains_list(virConnectPtr conn)
{
  virDomainPtr     *domains;
  int              num_domains, domain_counter, name_len;
  int              *Domain_ids;
  AllDomainsList_t *list = NULL;

  unsigned int flags = (VIR_CONNECT_LIST_DOMAINS_ACTIVE |
		       VIR_CONNECT_LIST_DOMAINS_RUNNING);

  num_domains = virConnectListAllDomains(conn, &domains, flags);

  if(num_domains > 0)
  {
    list = malloc(sizeof(AllDomainsList_t));
    list->count = num_domains;
    list->domain_info = calloc(num_domains, sizeof(DomainInfo_t));
    list->domains     = domains;
    /*member of domain_info will be re-freshed every X second (X=argv[1]) */

    /*List Active VM*/
    Domain_ids = malloc(sizeof(int) * num_domains);
    (void)virConnectListDomains(conn, Domain_ids, num_domains);

    #ifdef DEBUG_ENA
    printf("----- Write Domain List -----\n");
    #endif

    /*initialize domain_info for each domain*/
    for (domain_counter = 0; domain_counter < num_domains; domain_counter++) 
    {

     list->domain_info[domain_counter].domain_id = Domain_ids[domain_counter];

     name_len  = strlen(virDomainGetName(domains[domain_counter]));

     list->domain_info[domain_counter].domain_name = calloc(name_len, sizeof(char));

     strncpy(list->domain_info[domain_counter].domain_name, virDomainGetName(domains[domain_counter]), name_len);

    #ifdef DEBUG_ENA
     printf("ID:%d  %8s\n", list->domain_info[domain_counter].domain_id, list->domain_info[domain_counter].domain_name);
    #endif
    }
    free(Domain_ids);

  }
  else
  {
    fprintf(stderr, "No active domain, exit now...\n");
    exit(1);
  }

  return *list;

}


void myos_vcpu_scheduler_preprocess
(
  char           *cmd_line_param,

  int            *timer
)
{
  if(cmd_line_param == NULL)
  {
    fprintf(stderr, "No input argument, please execute in below way:\n");
    fprintf(stderr, "./vcpu_scheduler <input_argument>\n");
    fprintf(stderr, "exit...\n");
    exit(1);
  }

  *timer = atoi(cmd_line_param);

  if(*timer == 0)
  {
    fprintf(stderr, "invalid input argument = 0, please specify a non-zero number\n");
    fprintf(stderr, "exit...\n");
    exit(1);
  }

  fprintf(stderr, "CPU scheduler START...\n");
  fprintf(stderr, "User set refresh timer as %d \n", *timer);

}


void myos_open_local_connection
(
  virConnectPtr  *conn
)
{
  *conn = virConnectOpen("qemu:///system");
  if (*conn == NULL) 
  {
    fprintf(stderr, "Failed to open connection to qemu:///system\n");
    exit(1);
  }
  else
  {
    fprintf(stderr, "Successful connection to qemu:///system\n");
  }

}


void myos_fill_curr_domain_state
(
  virDomainStatsRecordPtr record,

  DomainInfo_t         *domain_info_ptr,

  DomainState_t        *domain_state_ptr
)
{
  int vcpus_count = 0, param_id;
  int domain_name_len;

  for (param_id = 0; param_id < record->nparams; param_id++) 
  {
    if (strcmp(record->params[param_id].field, "vcpu.current") == 0) 
    {
      vcpus_count = record->params[param_id].value.i;
    }
  }

  domain_state_ptr->domain_id = domain_info_ptr->domain_id;
  domain_name_len = strlen(domain_info_ptr->domain_name);
  strncpy(domain_state_ptr->domain_name, domain_info_ptr->domain_name, domain_name_len);

  domain_state_ptr->domain = record->dom;
  domain_state_ptr->num_vcpus = vcpus_count;

}



void myos_update_curr_domain_list_state
(
  int                  numActiveDomains,

  AllDomainsList_t     *domain_list_ptr,

  DomainListState_t    *list_state_ptr
)
{
  int          domain_counter;
  unsigned int query = 0;

  virDomainStatsRecordPtr *records = NULL;

  query = VIR_DOMAIN_STATS_VCPU;

  list_state_ptr->num_domain = numActiveDomains;

  for(domain_counter=0; domain_counter<numActiveDomains; domain_counter++)
  {
    if(virDomainListGetStats(domain_list_ptr->domains, query, &records, 0) > 0)
    {
      myos_fill_curr_domain_state(*records, &(domain_list_ptr->domain_info[domain_counter]), &(list_state_ptr->domain_state_param[domain_counter]));
    }
    else
    {
      fprintf(stderr, "Failed to get domain list state, exit...\n");
    }

  }

}

int myos_is_domain_list_state_changed
(
  DomainListState_t     *curr_state_ptr,
  DomainListState_t     *prev_state_ptr
)
{
  int state_changed = 0;

  if(curr_state_ptr->num_domain != prev_state_ptr->num_domain )
  {
    state_changed = 1;
  }
  
  return state_changed;
}



void myos_update_curr_vcpu_state
(
  DomainListState_t     *curr_state_ptr,	   
  int                   numActiveDomains,
  int                   max_pCPU
)
{

  unsigned char           pcpu_id_mask;
  int                     domain_counter, vcpu_counter, max_vCPU, pcpu_id = 0;
  virVcpuInfoPtr          vcpu_info;
  unsigned char           *pcpu_map;
  size_t                  pcpu_map_len;
  unsigned int            system_counter = 0;

  for (domain_counter = 0; domain_counter < numActiveDomains; domain_counter++) 
  {
    //printf("----- Domain info -----\n");
    printf("Domain ID:%d  %8s  vcpus:%d\n", curr_state_ptr->domain_state_param[domain_counter].domain_id, 
                                     curr_state_ptr->domain_state_param[domain_counter].domain_name,
                                     curr_state_ptr->domain_state_param[domain_counter].num_vcpus);

    max_vCPU = curr_state_ptr->domain_state_param[domain_counter].num_vcpus;
    vcpu_info = calloc(max_vCPU, sizeof(virVcpuInfo));
    pcpu_map_len = VIR_CPU_MAPLEN(max_pCPU);
    pcpu_map = calloc(max_vCPU, pcpu_map_len);

    printf("----- Load Default CPU mapping -----\n");
    if(virDomainGetVcpus(curr_state_ptr->domain_state_param[domain_counter].domain,
			 vcpu_info,
			 max_vCPU,
			 pcpu_map, 
                         pcpu_map_len) > 0)
    {

      for (vcpu_counter = 0; vcpu_counter < max_vCPU; vcpu_counter++) 
      {
        pcpu_id = (system_counter % max_pCPU);
        pcpu_id_mask = MYOS_GET_BITMASK(pcpu_id);
			
        virDomainPinVcpu(curr_state_ptr->domain_state_param[domain_counter].domain,
		          vcpu_counter,
		          &pcpu_id_mask,
		          pcpu_map_len);  


        /*Save the vCPU id, mapped pCPU id, cpuTime for this vCPU, as initial value to start with*/
        curr_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].vcpu_id         = vcpu_info[vcpu_counter].number;
        curr_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].mapped_pcpu_id  = pcpu_id;
        curr_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].vcpus_time_ns   = vcpu_info[vcpu_counter].cpuTime;

        system_counter++;

        printf("    vCPU %d, ID %d => pCPU ID %d, curr cpuTime %llu\n", vcpu_counter, vcpu_info[vcpu_counter].number, pcpu_id, vcpu_info[vcpu_counter].cpuTime);
 
      }
    }
    else
    {
      fprintf(stderr, "Failed to get vCPU info for domain: %8s, exit...\n", curr_state_ptr->domain_state_param[domain_counter].domain_name);
    }

    free(vcpu_info);
    free(pcpu_map);


    printf("\n");
  }

}


void myos_calc_domain_vcpus_pcpu_usage
(
  DomainListState_t     *curr_state_ptr,
  DomainListState_t     *prev_state_ptr,
  int                   numActiveDomains,
  int                   max_pCPU,
  long                  delta_time,
  double                *pcpu_usage
)
{
  int                     domain_counter, vcpu_counter, max_vCPU = 0;
  unsigned long long int  curr_time_ns, prev_time_ns, delta_time_llu, max_llu = 0;
  double                  usage_perc_temp;
  virVcpuInfoPtr          vcpu_info;
  unsigned char           *pcpu_map;
  size_t                  pcpu_map_len;
  int                     pcpu_counter, pcpu_id, num_vcpu_per_pcpu[max_pCPU]; 

  memset(num_vcpu_per_pcpu, 0, (max_pCPU * sizeof(int)));  

  for (domain_counter = 0; domain_counter < numActiveDomains; domain_counter++) 
  {

    printf("Domain ID:%d  %8s  vcpus:%d\n", curr_state_ptr->domain_state_param[domain_counter].domain_id, 
                                     curr_state_ptr->domain_state_param[domain_counter].domain_name,
                                     curr_state_ptr->domain_state_param[domain_counter].num_vcpus);

    max_vCPU = curr_state_ptr->domain_state_param[domain_counter].num_vcpus;

    vcpu_info = calloc(max_vCPU, sizeof(virVcpuInfo));
    pcpu_map_len = VIR_CPU_MAPLEN(max_pCPU);
    pcpu_map = calloc(max_vCPU, pcpu_map_len);

    if(virDomainGetVcpus(curr_state_ptr->domain_state_param[domain_counter].domain,
			 vcpu_info,
			 max_vCPU,
			 pcpu_map, 
                         pcpu_map_len) > 0)
    {

      printf("----- vCPU usage calculation -----\n");
      for (vcpu_counter = 0; vcpu_counter < max_vCPU; vcpu_counter++) 
      {

        curr_time_ns = vcpu_info[vcpu_counter].cpuTime;
        prev_time_ns = prev_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].vcpus_time_ns;

        /*Perform a few sanity checks before doing calculation*/
        /*1. cannot devided by 0*/
        assert(delta_time != 0);
 
        /*2. avoid llu overflow*/
        max_llu -= 1;
        delta_time_llu = (curr_time_ns >= prev_time_ns) ? (curr_time_ns - prev_time_ns) : (max_llu - prev_time_ns + curr_time_ns);

        usage_perc_temp = 100 * ((double)delta_time_llu/(double)delta_time);


        pcpu_id = vcpu_info[vcpu_counter].cpu;
        assert(pcpu_id < max_pCPU);

        pcpu_usage[pcpu_id] += usage_perc_temp;
        num_vcpu_per_pcpu[pcpu_id]++;

        /*Save curr vCPU info, to be used for reference in future*/
        curr_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].vcpu_id         = vcpu_info[vcpu_counter].number;
        curr_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].mapped_pcpu_id  = pcpu_id;
        curr_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].vcpus_time_ns   = vcpu_info[vcpu_counter].cpuTime;
        curr_state_ptr->domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].usage_perc      = usage_perc_temp;

        printf("    vCPU %d, ID %d => pCPU ID %d, curr cpuTime %llu, usage: %f%%\n", vcpu_counter, vcpu_info[vcpu_counter].number, pcpu_id, 
                                                                                   vcpu_info[vcpu_counter].cpuTime, usage_perc_temp);
 
      } //vCPU loop
    }
    else
    {
      fprintf(stderr, "Failed to get vCPU info for domain: %8s, exit...\n", curr_state_ptr->domain_state_param[domain_counter].domain_name);
    }

    free(vcpu_info);
    free(pcpu_map);

  }//domain loop

  printf("----- pCPU usage calculation -----\n");
  for (pcpu_counter = 0; pcpu_counter < max_pCPU; pcpu_counter++)
  {
    if(num_vcpu_per_pcpu[pcpu_counter] != 0)
    {
      pcpu_usage[pcpu_counter] /= ((double)num_vcpu_per_pcpu[pcpu_counter]);
    }
    else
    {
      pcpu_usage[pcpu_counter] = 0;
    }

    printf("    pCPU %d, vCPU assigned %d, usage: %f%%\n", pcpu_counter, num_vcpu_per_pcpu[pcpu_counter], pcpu_usage[pcpu_counter]);
  }

}

void myos_perfom_pcpu_realloc_if_needed
(
  DomainListState_t     *curr_state_ptr,
  int                   numActiveDomains,
  int                   max_pCPU,
  double                *pcpu_usage
)
{
  unsigned char perform_realloc = 0; 
  int freest_pcpu = 0, busiest_pcpu = 0, domain_counter, pcpu_counter, vcpu_counter;
  double freest_usage = 100.0;
  double busiest_usage = 0.0;

  for (pcpu_counter = 0; pcpu_counter < max_pCPU; pcpu_counter++) 
  {
    perform_realloc |= (pcpu_usage[pcpu_counter] > (double)USAGE_THRESHOLD);

    if (pcpu_usage[pcpu_counter] > busiest_usage) 
    {
      busiest_usage = pcpu_usage[pcpu_counter];
      busiest_pcpu = pcpu_counter;
    }
    
    if (pcpu_usage[pcpu_counter] < freest_usage) 
    {
      freest_usage = pcpu_usage[pcpu_counter];
      freest_pcpu = pcpu_counter;
    }
  }
	

  if (perform_realloc) 
  {
    printf("----- Perform Re-allocation (Threshold %f%%) -----\n", (double)USAGE_THRESHOLD);
    printf("    Busiest pCPU: %d vs Freest pCPU: %d\n", busiest_pcpu, freest_pcpu);

    virVcpuInfoPtr   vcpu_info;
    unsigned char    *pcpu_map;
    size_t           pcpu_map_len;
    unsigned char    freest_pcpu_map  = (0x1 << freest_pcpu);
    unsigned char    busiest_pcpu_map = (0x1 << busiest_pcpu);
    int              max_vCPU = 0;

    for (domain_counter = 0; domain_counter < numActiveDomains; domain_counter++) 
    {


      printf("Domain ID:%d  %8s  vcpus:%d\n", curr_state_ptr->domain_state_param[domain_counter].domain_id, 
                                     curr_state_ptr->domain_state_param[domain_counter].domain_name,
                                     curr_state_ptr->domain_state_param[domain_counter].num_vcpus);

      max_vCPU = curr_state_ptr->domain_state_param[domain_counter].num_vcpus;

      vcpu_info = calloc(max_vCPU, sizeof(virVcpuInfo));
      pcpu_map_len = VIR_CPU_MAPLEN(max_pCPU);
      pcpu_map = calloc(max_vCPU, pcpu_map_len);

      if(virDomainGetVcpus(curr_state_ptr->domain_state_param[domain_counter].domain,
			 vcpu_info,
			 max_vCPU,
			 pcpu_map, 
                         pcpu_map_len) > 0)
      {

        printf("----- vCPU=>pCPU re-map list -----\n");

        for (vcpu_counter = 0; vcpu_counter < max_vCPU; vcpu_counter++) 
        {
	  if (vcpu_info[vcpu_counter].cpu == busiest_pcpu) 
          {
	    printf("     Hot vCPU %d ===> Cold pCPU %d\n", vcpu_counter, freest_pcpu);

	    virDomainPinVcpu(curr_state_ptr->domain_state_param[domain_counter].domain,
			     vcpu_counter,
			     &freest_pcpu_map,
			     pcpu_map_len);

	  } 
          else if (vcpu_info[vcpu_counter].cpu == freest_pcpu) 
          {
	    printf("    Cold vCPU %d ===>  Hot pCPU %d\n", vcpu_counter, busiest_pcpu);

	    virDomainPinVcpu(curr_state_ptr->domain_state_param[domain_counter].domain,
			     vcpu_counter,
			     &busiest_pcpu_map,
			     pcpu_map_len);

	  }

	}//vCPU loop

      }
      else
      {
        fprintf(stderr, "Failed to get vCPU info for domain: %8s, exit...\n", curr_state_ptr->domain_state_param[domain_counter].domain_name);
      }
      
      free(vcpu_info);
      free(pcpu_map);

    }//domain loop
  }
  else
  {
    printf("No action needed...Busiest pCPU: %d, Freest pCPU: %d\n", busiest_pcpu, freest_pcpu);
  }

}


int main(int argc, char *argv[])
{
  virConnectPtr conn;

  AllDomainsList_t      all_domain_list;
  DomainListState_t     curr_domain_list_state;
  DomainListState_t     prev_domain_list_state;


  int max_pCPU, timer, numActiveDomains, domain_counter;
  char *hostname;
  double *pcpu_usage;

  /*****  Local initialization  *****/
  memset(&prev_domain_list_state, 0, sizeof(DomainListState_t));
  memset(&curr_domain_list_state, 0, sizeof(DomainListState_t));

  /*****  Pre-process the cmd line input   *****/
  myos_vcpu_scheduler_preprocess(argv[1], &timer);

  /*****  Open local connection   *****/
  myos_open_local_connection(&conn);

    hostname = virConnectGetHostname(conn);
    max_pCPU = virNodeGetCPUMap(conn, NULL, NULL, 0);

    if(hostname == NULL || max_pCPU == 0)
    {
      fprintf(stderr, "Failed to get the Host info, max_pCPU %d\n", max_pCPU);
    }
    else
    {  
      printf("----- Host Info -----\n");
      fprintf(stdout, "Hostname:%s  pCPU count: %d\n\n\n", hostname, max_pCPU);
      free(hostname);
    }


    while((numActiveDomains = (all_domain_list = myos_get_domains_list(conn)).count) > 0) 
    {
      myos_update_curr_domain_list_state(numActiveDomains, &all_domain_list, &curr_domain_list_state);

      #ifdef DEBUG_ENA
      printf("----- Read Domain List -----\n");
      #endif

      if (myos_is_domain_list_state_changed(&curr_domain_list_state, &prev_domain_list_state)) 
      {
        #ifdef DEBUG_ENA
        printf("\nDomainList state changed! Set initial value!\n");
        #endif
	/*Set initial vCPU -> pCPU mapping*/

	myos_update_curr_vcpu_state(&curr_domain_list_state,
			                      numActiveDomains,
			                      max_pCPU);

      } 
      else 
      {
        printf("\nNo DomainList state change, Perform usage calc...\n");

        /*Allocate memory to store pCPU usage in array*/
        pcpu_usage = calloc(max_pCPU, sizeof(double));

        /*Step 1, calculate vCPU and pCPU usage per each domain*/
        myos_calc_domain_vcpus_pcpu_usage(&curr_domain_list_state,
	        	                  &prev_domain_list_state,
			                  numActiveDomains,
		                          max_pCPU,
			                  ((long)timer * NANOSECOND),
                                          pcpu_usage);


        /* Step 2, find out the pCPU with max/min usage, and perform re-allocation*/
        myos_perfom_pcpu_realloc_if_needed(&curr_domain_list_state,
                                          numActiveDomains,
                                          max_pCPU,
                                          pcpu_usage);

       
        free(pcpu_usage);

      }

      for (domain_counter = 0; domain_counter < numActiveDomains; domain_counter++) 
      {
        #ifdef DEBUG_ENA
        printf("ID:%d  %8s\n", all_domain_list.domain_info[domain_counter].domain_id, all_domain_list.domain_info[domain_counter].domain_name);
        printf("ID:%d  %8s  vcpus:%d [in state db]\n", curr_domain_list_state.domain_state_param[domain_counter].domain_id, 
                                                       curr_domain_list_state.domain_state_param[domain_counter].domain_name,
                                                       curr_domain_list_state.domain_state_param[domain_counter].num_vcpus);


        printf("----- Read vCPU info -----\n");
        for(int vcpu_counter=0; vcpu_counter < curr_domain_list_state.domain_state_param[domain_counter].num_vcpus;vcpu_counter++)
        {
          printf("vCPU:%d  %llu\n", vcpu_counter, curr_domain_list_state.domain_state_param[domain_counter].vcpu_state_param[vcpu_counter].vcpus_time_ns);
        }
        #endif

        /*Domain loop clean-up*/
        all_domain_list.domain_info[domain_counter].domain_id = 0;
        free(all_domain_list.domain_info[domain_counter].domain_name);
        virDomainFree(all_domain_list.domains[domain_counter]);
        
      }//domain loop


      memcpy(&prev_domain_list_state, &curr_domain_list_state, sizeof(DomainListState_t));

      free(all_domain_list.domain_info);
      all_domain_list.count = 0;
      printf("\n\n");
      sleep(timer);
    }//end while
  
  virConnectClose(conn);
  free(conn);


  return 0;
}

