/* i386 libstdc++ tree nodes: color, parent, left, right (all 32 bits). */
typedef struct { uint32_t color,parent,left,right; } pg_node;
#define PN(x) ((pg_node *)(uintptr_t)(x))
static uint32_t pg_min(uint32_t x){while(PN(x)->left)x=PN(x)->left;return x;}
static uint32_t pg_max(uint32_t x){while(PN(x)->right)x=PN(x)->right;return x;}
static void pg_rotate(uint32_t x,uint32_t h,bool left)
{
    uint32_t y=left?PN(x)->right:PN(x)->left;
    uint32_t child=left?PN(y)->left:PN(y)->right;
    if(left)PN(x)->right=child;else PN(x)->left=child;
    if(child)PN(child)->parent=x;
    uint32_t p=PN(x)->parent;PN(y)->parent=p;
    if(p==h)PN(h)->parent=y;else if(PN(p)->left==x)PN(p)->left=y;else PN(p)->right=y;
    if(left)PN(y)->left=x;else PN(y)->right=x;
    PN(x)->parent=y;
}
static void pg_replace(uint32_t h,uint32_t u,uint32_t v){uint32_t p=PN(u)->parent;if(p==h)PN(h)->parent=v;else if(u==PN(p)->left)PN(p)->left=v;else PN(p)->right=v;if(v)PN(v)->parent=p;}
static int peggle_tree_dispatch(const char *n,const uint32_t *a,uint64_t *r)
{
    if(strstr(n,"_Rb_tree_insert_and_rebalance")){
        uint32_t x=a[1],p=a[2],h=a[3];*PN(x)=(pg_node){0,p,0,0};
        if(a[0]){PN(p)->left=x;if(p==h){PN(h)->parent=x;PN(h)->right=x;}else if(PN(h)->left==p)PN(h)->left=x;}
        else {PN(p)->right=x;if(PN(h)->right==p)PN(h)->right=x;}
        while(x!=PN(h)->parent && PN(PN(x)->parent)->color==0){p=PN(x)->parent;uint32_t g=PN(p)->parent;bool l=p==PN(g)->left;uint32_t u=l?PN(g)->right:PN(g)->left;
            if(u&&PN(u)->color==0){PN(p)->color=PN(u)->color=1;PN(g)->color=0;x=g;}
            else{if(x==(l?PN(p)->right:PN(p)->left)){x=p;pg_rotate(x,h,l);p=PN(x)->parent;g=PN(p)->parent;}PN(p)->color=1;PN(g)->color=0;pg_rotate(g,h,!l);}}
        PN(PN(h)->parent)->color=1;*r=0;return 1;
    }
    if(strstr(n,"_Rb_tree_increment")){uint32_t x=a[0];if(PN(x)->right)x=pg_min(PN(x)->right);else{uint32_t y=PN(x)->parent;while(x==PN(y)->right){x=y;y=PN(y)->parent;}if(PN(x)->right!=y)x=y;}*r=x;return 1;}
    if(strstr(n,"_Rb_tree_decrement")){uint32_t x=a[0];if(PN(x)->color==0&&PN(PN(x)->parent)->parent==x)x=PN(x)->right;else if(PN(x)->left)x=pg_max(PN(x)->left);else{uint32_t y=PN(x)->parent;while(x==PN(y)->left){x=y;y=PN(y)->parent;}x=y;}*r=x;return 1;}
    if(strstr(n,"_Rb_tree_rebalance_for_erase")){
        uint32_t z=a[0],h=a[1],y=z,x,p;uint32_t color=PN(y)->color;
        if(!PN(z)->left){x=PN(z)->right;p=PN(z)->parent;pg_replace(h,z,x);}
        else if(!PN(z)->right){x=PN(z)->left;p=PN(z)->parent;pg_replace(h,z,x);}
        else {y=pg_min(PN(z)->right);color=PN(y)->color;x=PN(y)->right;if(PN(y)->parent==z){p=y;if(x)PN(x)->parent=y;}else{p=PN(y)->parent;pg_replace(h,y,x);PN(y)->right=PN(z)->right;PN(PN(y)->right)->parent=y;}pg_replace(h,z,y);PN(y)->left=PN(z)->left;PN(PN(y)->left)->parent=y;PN(y)->color=PN(z)->color;}
        if(color==1)while(x!=PN(h)->parent&&(!x||PN(x)->color==1)){
            bool l=x==PN(p)->left;uint32_t w=l?PN(p)->right:PN(p)->left;
            if(!w){x=p;p=PN(x)->parent;continue;}
            if(PN(w)->color==0){PN(w)->color=1;PN(p)->color=0;pg_rotate(p,h,l);w=l?PN(p)->right:PN(p)->left;}
            uint32_t near=l?PN(w)->left:PN(w)->right,far=l?PN(w)->right:PN(w)->left;
            if((!near||PN(near)->color==1)&&(!far||PN(far)->color==1)){PN(w)->color=0;x=p;p=PN(x)->parent;}
            else{if(!far||PN(far)->color==1){if(near)PN(near)->color=1;PN(w)->color=0;pg_rotate(w,h,!l);w=l?PN(p)->right:PN(p)->left;far=l?PN(w)->right:PN(w)->left;}PN(w)->color=PN(p)->color;PN(p)->color=1;if(far)PN(far)->color=1;pg_rotate(p,h,l);x=PN(h)->parent;break;}}
        if(x)PN(x)->color=1;uint32_t root=PN(h)->parent;PN(h)->left=root?pg_min(root):h;PN(h)->right=root?pg_max(root):h;*r=z;return 1;
    }
    if(strstr(n,"_List_node_base4hook")){uint32_t *x=(void *)(uintptr_t)a[0],*p=(void *)(uintptr_t)a[1];x[0]=a[1];x[1]=p[1];((uint32_t *)(uintptr_t)p[1])[0]=a[0];p[1]=a[0];*r=0;return 1;}
    if(strstr(n,"_List_node_base6unhook")){uint32_t *x=(void *)(uintptr_t)a[0];((uint32_t *)(uintptr_t)x[0])[1]=x[1];((uint32_t *)(uintptr_t)x[1])[0]=x[0];*r=0;return 1;}
    return 0;
}
#undef PN
